#include "JZWifiDriverEsp32.h"
#include "JZWifi.h"
#include "JZDebug.h"
#include "JZThread.h"    // runOnObject: 把扫描发起派发回扫描线程(事件回调里不能裸调 WiFi API)

extern "C" {
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <arpa/inet.h>
}

// 把点分十进制 IP 解析为网络字节序的 uint32 (与 esp_netif ip_info 字段保持一致)
static uint32_t parseIp(const std::string &s)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return htonl((a << 24) | (b << 16) | (c << 8) | d);  // htonl 转为网络字节序
    return 0;
}

// 把网络字节序 IP 转为点分十进制字符串
static std::string ipToStr(uint32_t ip)
{
    uint32_t host = ntohl(ip);  // 先转为主机字节序再提取各字段
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)((host >> 24) & 0xff), (unsigned)((host >> 16) & 0xff),
             (unsigned)((host >> 8) & 0xff), (unsigned)(host & 0xff));
    return std::string(buf);
}

static void macToStr(const uint8_t *mac, char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 把 "xx:xx:xx:xx:xx:xx" 解析为 6 字节 MAC, 成功返回 true
static bool strToMac(const std::string &s, uint8_t mac[6])
{
    unsigned b[6] = {0};
    if (sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++)
            mac[i] = (uint8_t)b[i];
        return true;
    }
    return false;
}

// ==================== 事件回调 (注册到 esp_event) ====================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    auto *self = static_cast<JZWifiDriverEsp32 *>(arg);
    self->handleEvent(event_base, event_id, event_data);
}

// ==================== 构造/析构 ====================
JZWifiDriverEsp32::JZWifiDriverEsp32()
{
    // 连接切换超时定时器: stationSwitch 中途连不上时, 由 onSwitchTimeout 复位 m_switching 并上报失败
    connect(&m_switchTimer.sigTimeout, this, &JZWifiDriverEsp32::onSwitchTimeout);
    m_switchTimer.setSingleShot(true);
}

JZWifiDriverEsp32::~JZWifiDriverEsp32()
{
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    if (m_started)
        esp_wifi_stop();
    esp_wifi_deinit();
}

void JZWifiDriverEsp32::ensureInit()
{
    if (m_inited)
        return;

    m_inited = true;    

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, this);
}

bool JZWifiDriverEsp32::ensureStarted()
{
    if (m_started)
        return true;

    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        m_started = true;
        return true;
    }

    // 启动失败不置 m_started, 下次调用可重试; 同时打日志便于定位
    // (scan/connect 在此之前调用会得到 ESP_ERR_WIFI_NOT_START 等错误)。
    m_staStarted = false;
    jzWarning() << "JZWifiDriverEsp32::ensureStarted failed" << err;
    return false;
}

// 根据 sta/ap 的请求状态重算 WiFi 工作模式
bool JZWifiDriverEsp32::updateMode()
{
    bool enable_ap = false;
    bool enable_sta = false;
    {
        auto obj = (JZWifiAp *)m_apWifi.object();
        if (obj)
            enable_ap = (obj->state() == JZWifiAp::Starting || obj->state() == JZWifiAp::Started);
    }

    {
        auto obj = (JZWifiStation *)m_staWifi.object();
        if (obj)
            enable_sta = (obj->state() == JZWifiStation::Connecting || obj->state() == JZWifiStation::Connected);
    }

    // 扫描也需要 STA 射频: 即使没有 sta 连接对象, 只要有人在扫描(或等待 STA 就绪),
    // 就必须使能 STA, 否则扫描无法在纯 AP 模式下进行(也不会误杀正在运行的 AP)。
    if (m_scanTarget || m_scanPending)
        enable_sta = true;

    wifi_mode_t mode;
    if (enable_sta && enable_ap)
        mode = WIFI_MODE_APSTA;
    else if (enable_sta)
        mode = WIFI_MODE_STA;
    else if (enable_ap)
        mode = WIFI_MODE_AP;
    else {
        if (m_started) {
            esp_wifi_stop();
            m_started = false;
            m_staStarted = false;
        }
        return true;
    }

    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        jzWarning() << "JZWifiDriverEsp32::updateMode: set_mode" << (int)mode << "failed" << err;
        return false;
    }
    return true;
}

// ==================== 事件分发 ====================
void JZWifiDriverEsp32::handleEvent(const char *event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            // STA 真正就绪(esp_wifi_start 是异步的, 此事件后才可扫描/连接)。
            m_staStarted = true;
            // 若有等待 STA 就绪的待发扫描, 通过 runOnObject 把 "真正发起扫描" 派发回
            // 扫描线程执行(不可在事件回调里裸调 esp_wifi_scan_start, 会重入 WiFi API 死锁)。
            if (m_scanPending) {
                m_scanPending = false;
                runOnObject(this, [this]() { this->doScanStart(); });
            }
            break;

        case WIFI_EVENT_STA_STOP:
            m_staStarted = false;
            break;

        case WIFI_EVENT_STA_CONNECTED:
            // 仅表示链路层关联成功, 不设 m_staUp; 等拿到 IP 才算真正连接
            jzDebug() << "wifi event WIFI_EVENT_STA_CONNECTED";
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            jzDebug() << "wifi event WIFI_EVENT_STA_DISCONNECTED";
            if (m_switching) {
                // 切换进行中: 中间断开(esp_wifi_disconnect 触发)立即视为切换失败,
                // 经 onStationSwitched(失败) 上报, 不再另发 onStationDisconnected。
                // 去掉了 15s 超时定时器: 断线即时反馈, 无空窗等待。
                JZWifiDriver::onStationSwitched(m_staWifi, false, "");
                m_switching = false;
                m_staWifi = JZObjectPointer();
                break;
            }
            if (m_staWifi) {
                JZWifiDriver::onStationDisconnected(m_staWifi);
                m_staWifi = JZObjectPointer();
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            m_apUp = true;
            if (m_apWifi)
                JZWifiDriver::onApStarted(m_apWifi);
            break;

        case WIFI_EVENT_AP_STOP:
            m_apUp = false;
            if (m_apWifi)
            {
                JZWifiDriver::onApStopped(m_apWifi);
                m_apWifi = JZObjectPointer();
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            auto *e = static_cast<wifi_event_ap_staconnected_t *>(event_data);
            char mac[18];
            macToStr(e->mac, mac);
            if (m_apWifi)
                JZWifiDriver::onApStationConnected(m_apWifi, mac, "");
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            auto *e = static_cast<wifi_event_ap_stadisconnected_t *>(event_data);
            char mac[18];
            macToStr(e->mac, mac);
            if (m_apWifi)            
                JZWifiDriver::onApStationDisconnected(m_apWifi, mac, "");            
            break;
        }

        case WIFI_EVENT_SCAN_DONE: {                
            if (!m_scanTarget)
                break;
                
            uint16_t ap_num = 0;
            JZWifiScanResult ap_result;
            if (esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK && ap_num > 0) {
                std::vector<wifi_ap_record_t> aps(ap_num);
                if (esp_wifi_scan_get_ap_records(&ap_num, aps.data()) == ESP_OK) {
                    for (uint16_t i = 0; i < ap_num; i++) {
                        JZWifiScanResult::ApInfo r;
                        r.ssid = reinterpret_cast<const char *>(aps[i].ssid);
                        char mac[18];
                        macToStr(aps[i].bssid, mac);
                        r.bssid = mac;
                        r.rssi = aps[i].rssi;
                        r.channel = aps[i].primary;
                        r.authMode = aps[i].authmode;
                        ap_result.apList.push_back(r);
                    }
                }
            }
            JZWifiDriver::onScanFinished(m_scanTarget, ap_result);
            m_scanTarget = JZObjectPointer();
            break;
        }

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP: {
            jzDebug() << "wifi event IP_EVENT_STA_GOT_IP";
            auto *e = static_cast<ip_event_got_ip_t *>(event_data);
            std::string staIp = ipToStr(e->ip_info.ip.addr);
            // 取当前已连 AP 的 BSSID, 随连接事件一起回传(避免单独的实时查询接口)
            std::string staBssid;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                char mac[18];
                macToStr(ap.bssid, mac);
                staBssid = mac;
            }
            if (m_staWifi)
            {
                if (m_switching) {
                    // 切换成功: 经 onStationSwitched(成功) 上报, 不再另发 onStationConnected
                    JZWifiDriver::onStationSwitched(m_staWifi, true, staBssid.c_str());
                } else {
                    JZWifiDriver::onStationConnected(m_staWifi, staIp.c_str(), staBssid.c_str());
                }
            }
            m_switching = false;
            break;
        }

        default:
            break;
        }
    }
}

// ==================== Station (连接) ====================
void JZWifiDriverEsp32::stationConnect(JZWifiStation *w, const JZWifiConfig &cfg)
{
    ensureInit();
    JZ_ASSERT_X(!m_staWifi, "sta wifi allready exist");

    m_staWifi = w->guardPointer();    

    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char *>(wc.sta.ssid), cfg.ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wc.sta.password), cfg.password.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = cfg.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    // 指定目标 AP 的 BSSID: 用于同 SSID 漫游时钉选更强的那个物理 AP
    if (!cfg.bssid.empty()) {
        uint8_t mac[6];
        if (strToMac(cfg.bssid, mac)) {
            wc.sta.bssid_set = 1;
            memcpy(wc.sta.bssid, mac, sizeof(mac));
        } else {
            jzWarning() << "JZWifiDriverEsp32: invalid bssid" << cfg.bssid.c_str();
        }
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    if (!cfg.dhcp) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_dhcpc_stop(sta);
            esp_netif_ip_info_t ipinfo;
            memset(&ipinfo, 0, sizeof(ipinfo));
            ipinfo.ip.addr = parseIp(cfg.ip);
            ipinfo.gw.addr = parseIp(cfg.gateway);
            ipinfo.netmask.addr = parseIp(cfg.subnet);
            esp_netif_set_ip_info(sta, &ipinfo);
        }
    }

    updateMode();
    ensureStarted();
    esp_wifi_connect();
}

void JZWifiDriverEsp32::stationDisconnect(JZWifiStation *w)
{        
    esp_wifi_disconnect();
    updateMode();
}

void JZWifiDriverEsp32::stationSwitch(JZWifiStation *w, const JZWifiConfig &cfg)
{
    // 已连接态切换到另一 AP(同 SSID 更强 BSSID 或不同 SSID):
    // 更新目标配置并重新连接。中间断开事件由断线状态机吞掉(不在此上报);
    // 切换结果由 GOT_IP(成功复位) 或切换超时定时器(失败上报) 决定。
    // 注意: 不重设 m_staWifi(仍绑定同一个 JZWifiStation 对象, stationConnect 的断言不适用)。
    ensureInit();
    ensureStarted();
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char *>(wc.sta.ssid), cfg.ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wc.sta.password), cfg.password.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = cfg.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    if (!cfg.bssid.empty()) {
        uint8_t mac[6];
        if (strToMac(cfg.bssid, mac)) {
            wc.sta.bssid_set = 1;
            memcpy(wc.sta.bssid, mac, sizeof(mac));
        } else {
            jzWarning() << "JZWifiDriverEsp32: invalid bssid" << cfg.bssid.c_str();
        }
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    if (!cfg.dhcp) {
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) {
            esp_netif_dhcpc_stop(sta);
            esp_netif_ip_info_t ipinfo;
            memset(&ipinfo, 0, sizeof(ipinfo));
            ipinfo.ip.addr = parseIp(cfg.ip);
            ipinfo.gw.addr = parseIp(cfg.gateway);
            ipinfo.netmask.addr = parseIp(cfg.subnet);
            esp_netif_set_ip_info(sta, &ipinfo);
        }
    }

    // 重新连接: 先断开当前 AP, 再连到新 AP
    m_switching = true;
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // esp_wifi_connect 同步失败: 直接视为切换失败, 经 onStationSwitched(失败) 上报
        m_switching = false;
        if (m_staWifi) {
            JZWifiDriver::onStationSwitched(m_staWifi, false, "");
            m_staWifi = JZObjectPointer();
        }
        return;
    }
    // 切换结果由事件驱动: GOT_IP -> onStationSwitched(成功); 中间断开 -> onStationSwitched(失败)。
    // 去掉 15s 超时定时器: 断线即时反馈, 无需空窗等待; 也不再吞掉中间断开。
}

bool JZWifiDriverEsp32::isStationConnected(JZWifiStation *w)
{
    return w->isConnected();
}

std::string JZWifiDriverEsp32::stationIpAddress(JZWifiStation *w)
{
    return w->ipAddress();
}

int JZWifiDriverEsp32::stationRssi(JZWifiStation *w)
{
    (void)w;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return -127;
}

// ==================== Access Point (做服务) ====================
void JZWifiDriverEsp32::apStart(JZWifiAp *w, const JZWifiApConfig &cfg)
{
    ensureInit();
    JZ_ASSERT_X(!m_apWifi, "ap allready exists");

    m_apWifi = w->guardPointer();    

    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char *>(wc.ap.ssid), cfg.ssid.c_str(), sizeof(wc.ap.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wc.ap.password), cfg.password.c_str(), sizeof(wc.ap.password) - 1);
    wc.ap.ssid_len = 0;
    wc.ap.channel = (uint8_t)cfg.channel;
    wc.ap.max_connection = (uint8_t)cfg.maxConnections;
    wc.ap.authmode = cfg.password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.ap.ssid_hidden = cfg.hidden ? 1 : 0;
    esp_wifi_set_config(WIFI_IF_AP, &wc);

    // 设置 AP 自身 IP / 子网 / 网关, 并重启 DHCP 服务器
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap) {
        esp_netif_dhcps_stop(ap);
        esp_netif_ip_info_t ipinfo;
        memset(&ipinfo, 0, sizeof(ipinfo));
        ipinfo.ip.addr = parseIp(cfg.ip);
        ipinfo.gw.addr = parseIp(cfg.gateway);
        ipinfo.netmask.addr = parseIp(cfg.subnet);
        esp_netif_set_ip_info(ap, &ipinfo);
        esp_netif_dhcps_start(ap);
    }
    
    updateMode();
    ensureStarted();
}

void JZWifiDriverEsp32::apStop(JZWifiAp *w)
{
    (void)w;
    updateMode();
}

bool JZWifiDriverEsp32::isApRunning(JZWifiAp *w)
{
    return w ? w->isRunning() : m_apUp;
}

int JZWifiDriverEsp32::apStationCount(JZWifiAp *w)
{
    return w ? w->connectedStationCount() : 0;
}

// ==================== Scan (扫描) ====================
void JZWifiDriverEsp32::scanStart(JZWifiScanner *s, const JZScanConfig &cfg)
{
    JZ_ASSERT(!m_scanTarget);

    ensureInit();

    // 先登记扫描态, 使 updateMode() 能感知到"正在扫描"并据此使能 STA 射频
    // (纯 AP 模式下也会升级为 APSTA, 不会误杀正在运行的 AP)。
    m_scanTarget = s->guardPointer();
    m_scanCfg = cfg;

    if (!ensureStarted()) {
        // esp_wifi_start() 已经明确失败, 无需再轮询等待 m_staStarted(它必然不会置位),
        // 直接异步上报扫描错误, 由上层退避重试。避免无效阻塞 500ms。
        scanError(s->guardPointer(), (int)ESP_ERR_WIFI_NOT_STARTED, "wifi start failed");
        return;
    }

    // 由统一的 updateMode() 计算并设置 WiFi 模式(已感知扫描态)。
    // set_mode 失败立即上报, 不再向下(否则 esp_wifi_scan_start 会以模糊 ESP_FAIL 失败)。
    if (!updateMode()) {
        scanError(s->guardPointer(), (int)ESP_FAIL, "set wifi mode failed");
        return;
    }

    if (m_staStarted) {
        doScanStart();
    } else {
        m_scanPending = true;
        // 注意: 此处不阻塞等待。若 STA_START 长时间不来, 上层应自行超时(scanStop/重试)。
    }
}

void JZWifiDriverEsp32::scanError(const JZObjectPointer &scanner, int err, const char *msg)
{
    jzWarning() << "JZWifiDriverEsp32::scanError:" << msg << err;
    JZWifiDriver::onScanError(scanner, err, msg);
    // 失败/异常必须复位扫描态: 否则 m_scanTarget 残留, 上层 m_active 已被复位,
    // 下一次 scanStart 会因 "m_scanTarget 非空" 触发断言崩溃。
    m_scanTarget = JZObjectPointer();
    m_scanPending = false;
}

void JZWifiDriverEsp32::doScanStart()
{
    // 防御: m_scanTarget 可能已被 scanStop/超时复位, 此时放弃发起。
    if (!m_scanTarget) {
        jzWarning() << "JZWifiDriverEsp32::doScanStart: target already reset, skip";
        return;
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.ssid = nullptr;
    scan_cfg.bssid = nullptr;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = false;

    // 低影响漫游扫描: 减小对已建立 WiFi 链路的打扰(避免断流/卡顿)
    // roamScan 由调用方在 JZScanConfig 中指定(见 JZWifiScanner/AutoConnectService)。
    // 注: 实际 scan_cfg 字段填充需 cfg, 调用方把 cfg 透传进来。下面由 m_scanCfg 提供。
    {
        JZScanConfig &cfg = m_scanCfg;
        if (cfg.roamScan) {
            // 1) passive 扫描: 仅监听 beacon, 不发 probe request, 降低对链路的打扰
            scan_cfg.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        }
        // 2) 仅扫当前信道: 射频不必离开 home channel, 进一步避免业务中断
        if (!cfg.scanAllChannels) {
            uint8_t cur_chan = 0;
            wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
            if (esp_wifi_get_channel(&cur_chan, &second) == ESP_OK && cur_chan != 0)
                scan_cfg.channel = cur_chan;
        }
        // 3) 按 SSID 过滤(漫游时聚焦已连 SSID, 结果更干净); 空则不限制
        if (!cfg.ssid.empty()) {
            m_scanSsid = cfg.ssid;
            scan_cfg.ssid = (uint8_t *)m_scanSsid.c_str();
        }
    }

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        // 失败必须复位扫描态, 否则残留会让下次 start 误判"正在扫描"触发断言。
        scanError(m_scanTarget, (int)err, "esp_wifi_scan_start failed");
    }
}

void JZWifiDriverEsp32::scanStop(JZWifiScanner *s)
{
    JZ_ASSERT(m_scanTarget);

    esp_wifi_scan_stop();
    // 主动复位, 不依赖 WIFI_EVENT_SCAN_DONE(STOPPED) 事件到达: 该事件可能晚到
    // 甚至丢失, 残留 m_scanTarget 会导致下一次 scanStart 误判"正在扫描"。
    m_scanTarget = JZObjectPointer();
    m_scanPending = false;  // 取消可能等待 STA_START 触发的待发扫描

    // 扫描是临时借用 STA 射频, 结束时重算模式: updateMode() 幂等, 目标模式与当前一致时
    // 为空操作; 无 sta/ap 在跑则回收 WiFi(esp_wifi_stop)省电, 有则保持对应模式。
    updateMode();
}

bool JZWifiDriverEsp32::isScanning(JZWifiScanner *s)
{
    return s ? (s == (JZWifiScanner *)m_scanTarget.object()): false;
}
