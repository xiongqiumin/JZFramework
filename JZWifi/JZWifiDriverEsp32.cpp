#include "JZWifiDriverEsp32.h"
#include "JZWifi.h"
#include "JZDebug.h"

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

}

JZWifiDriverEsp32::~JZWifiDriverEsp32()
{
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    if (m_started)
        esp_wifi_stop();
    esp_wifi_deinit();
}

bool JZWifiDriverEsp32::isWifiAvailable() 
{ 
    return true; 
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

void JZWifiDriverEsp32::ensureStarted()
{
    if (!m_started) {
        esp_wifi_start();
        m_started = true;
    }
}

// 根据 sta/ap 的请求状态重算 WiFi 工作模式
void JZWifiDriverEsp32::updateMode()
{
    bool enable_ap = false;
    bool enable_sta = false;
    {                
        auto obj = (JZWifiAp *)m_apWifi.object();
        if(obj)
            enable_ap = (obj->state() == JZWifiAp::Starting || obj->state() == JZWifiAp::Started); 
    }

    {        
        auto obj = (JZWifiStation *)m_staWifi.object();
        if(obj)
            enable_sta = (obj->state() == JZWifiStation::Connecting || obj->state() == JZWifiStation::Connected); 
    }

    wifi_mode_t mode;
    if (enable_sta && enable_ap)
        mode = WIFI_MODE_APSTA;
    else if (enable_sta)
        mode = WIFI_MODE_STA;
    else if (enable_ap)
        mode = WIFI_MODE_AP;
    else {
        if (m_started)
        {
            esp_wifi_stop();
            m_started = false;
        }
        return;
    }
    esp_wifi_set_mode(mode);
}

// ==================== 事件分发 ====================
void JZWifiDriverEsp32::handleEvent(const char *event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_CONNECTED:
            // 仅表示链路层关联成功, 不设 m_staUp; 等拿到 IP 才算真正连接
            jzDebug() << "wifi event WIFI_EVENT_STA_CONNECTED";
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            jzDebug() << "wifi event WIFI_EVENT_STA_DISCONNECTED";
            if (m_staWifi)
            {
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
            if (m_staWifi)
            {
                JZWifiDriver::onStationConnected(m_staWifi, staIp.c_str());
            }
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
void JZWifiDriverEsp32::scanStart(JZWifiScanner *s)
{
    ensureInit();
    ensureStarted();

    JZ_ASSERT_X(!m_scanTarget, "wify allready start scan");
    
    m_scanTarget = s->guardPointer();    
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.ssid = nullptr;
    scan_cfg.bssid = nullptr;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        jzWarning() << "JZWifiDriverEsp32::scanStart failed" << err;        
        JZWifiDriver::onScanError(m_scanTarget, (int)err, "esp_wifi_scan_start failed");
    }
}

void JZWifiDriverEsp32::scanStop(JZWifiScanner *s)
{
    JZ_ASSERT_X(s == m_scanTarget.object(),"scanStop invalid param");

    if (m_scanTarget) {
        esp_wifi_scan_stop();
    }    
}

bool JZWifiDriverEsp32::isScanning(JZWifiScanner *s)
{
    return s ? (s == (JZWifiScanner *)m_scanTarget.object()): false;
}
