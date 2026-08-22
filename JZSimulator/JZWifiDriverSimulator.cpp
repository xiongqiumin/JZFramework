#include "JZWifiDriverSimulator.h"
#include "JZDebug.h"
#include "JZNetworkInterface.h"
#include "JZSignal.h"
#include "JZApplication.h"

JZ_OBJECT_IMPL(JZWifiDriverSimulator, JZObject)

namespace
{
// 模拟驱动返回的信号强度, 仅用于让上层拿到一个"看起来正常"的值。
const int kSimRssi = -50;   // dBm, 对应 signalStrength() = 4 (良好)

// 取本机首个活动网卡的 IPv4 地址(通过 JZNetworkInterface), 取不到时回退 127.0.0.1。
std::string localIp()
{
    auto ifs = JZNetworkInterface::allInterfaces();
    if (!ifs.empty())
        return ifs.front().ip;
    return "127.0.0.1";
}
} // namespace

// ==================== 构造 / 析构 ====================
JZWifiDriverSimulator::JZWifiDriverSimulator(const std::string &ip, int port)
{
    (void)ip;
    (void)port;
    m_sim = JZSimServer::instance();
    m_simToken = m_sim->registerSimulator(
        [this](JZSimCmd cmd) { return support(cmd); },
        [this](const JZSimPack &pack) { onRecv(pack); });
    m_sim->start("wifi");
}

JZWifiDriverSimulator::~JZWifiDriverSimulator()
{
    if (m_sim)
    {
        m_sim->unregisterSimulator(m_simToken);
        m_sim->stop("wifi");
    }
}

bool JZWifiDriverSimulator::support(JZSimCmd cmd) const
{
    int c = (int)cmd;
    return c >= 200 && c < 300; // SIM_WIFI_CMD_BASE .. SIM_WIFI_CMD_BASE+100
}

bool JZWifiDriverSimulator::isWifiAvailable()
{
    return true;
}

void JZWifiDriverSimulator::sendApCmd(const JZSimPack &pack)
{
    sigSim.emit(pack.cmd, pack.data);
    m_sim->sendSim(pack.cmd, pack.data);
}

JZWifiDriverSimulator *JZWifiDriverSimulator::simInstance()
{
    return dynamic_cast<JZWifiDriverSimulator *>(JZWifiDriver::instance());
}


// ==================== Station (连接) ====================
// 连接规则: 凭证须匹配服务端配置(config()["wifi"]["aps"])中的某个 AP; 否则失败。
// 返回匹配 AP 的 bssid(配置里若提供), 否则回退默认 bssid。
static std::string matchApBssid(const JZWifiConfig &cfg)
{
    const Json::Value& wifiCfg = JZSimServer::instance()->config()["wifi"];
    const Json::Value& aps = (wifiCfg.isObject() && wifiCfg.isMember("aps")) ? wifiCfg["aps"] : Json::Value(Json::arrayValue);
    if (aps.isArray())
    {
        for (auto it = aps.begin(); it != aps.end(); ++it)
        {
            const Json::Value& a = *it;
            std::string ssid = a.isMember("ssid") ? a["ssid"].asString() : std::string();
            std::string pwd  = a.isMember("password") ? a["password"].asString() : std::string();
            if (ssid == cfg.ssid && pwd == cfg.password)
                return a.isMember("bssid") ? a["bssid"].asString() : std::string();
        }
    }
    return std::string();   // 未匹配
}

// Station 连接不再"默认成功": 仅当凭证匹配配置中的 AP 才向模拟器发起连接请求,
// 真正是否连上由模拟器经 onRecv(SIM_WIFI_STATION_RESULT) 回发决定。
void JZWifiDriverSimulator::stationConnect(JZWifiStation *w, const JZWifiConfig &cfg)
{
    std::string bssid = matchApBssid(cfg);
    if (bssid.empty())
    {
        JZWifiDriver::onStationError(w->guardPointer(), -1, "invalid ssid or password");
        return;
    }

    // 记录待确认请求(不在本地直接视为成功, ssid/password 以回包为准)。
    m_staWifi = w->guardPointer();

    JZSimPack pack;
    pack.cmd = SIM_WIFI_STATION_CONNECT;
    pack.setStr("ssid", cfg.ssid);
    pack.setStr("password", cfg.password);
    pack.setStr("bssid", bssid);
    m_sim->addAssoc("wifi", "station", "WiFi station " + cfg.ssid);
    sendApCmd(pack);
}

void JZWifiDriverSimulator::stationDisconnect(JZWifiStation *w)
{
    // 无论本地是否处于已连接态, 都向模拟器发断开包, 保证 start/stop 状态机配对正确
    // (即使在回包未到、本地 m_staWifi 已被清理的情况下也通知模拟器, 避免状态错位)。
    JZSimPack pack;
    pack.cmd = SIM_WIFI_STATION_DISCONNECT;
    pack.setStr("ssid", m_staSsid);
    sendApCmd(pack);

    m_staWifi = JZObjectPointer();
    m_staSsid.clear();
    m_staPassword.clear();
    m_sim->removeAssoc("wifi", "station");
    JZWifiDriver::onStationDisconnected(w->guardPointer());
}

void JZWifiDriverSimulator::stationSwitch(JZWifiStation *w, const JZWifiConfig &cfg)
{
    // 切换 = 先断开当前 station, 再以新凭证发起连接(经模拟器确认, 不本地默认成功)。
    std::string bssid = matchApBssid(cfg);
    if (bssid.empty())
    {
        // 新凭证不匹配任何配置 AP: 切换失败, 视为断线(由上层按真实掉线处理, 触发重连)。
        m_staWifi = JZObjectPointer();
        JZWifiDriver::onStationDisconnected(w->guardPointer());
        return;
    }
    // 复用 stationConnect 的"经模拟器确认"流程: 先清旧连接态, 再发起新连接请求。
    m_staWifi = JZObjectPointer();
    m_staSsid.clear();
    m_staPassword.clear();
    stationConnect(w, cfg);
}

bool JZWifiDriverSimulator::isStationConnected(JZWifiStation *w)
{
    (void)w;
    return true;
}

std::string JZWifiDriverSimulator::stationIpAddress(JZWifiStation *w)
{
    (void)w;
    return localIp();
}

int JZWifiDriverSimulator::stationRssi(JZWifiStation *w)
{
    (void)w;
    return kSimRssi;
}

// ==================== Access Point (做服务) ====================
void JZWifiDriverSimulator::apStart(JZWifiAp *w, const JZWifiApConfig &cfg)
{
    m_apWifi = w->guardPointer();

    // 使用 JZSimPack 成员 API 构造上行包(JZResult / 多级 key 在此统一提供)
    JZSimPack pack;
    pack.cmd = SIM_WIFI_AP_START;
    pack.setStr("ap_ssid", cfg.ssid);
    pack.setStr("ap_password", cfg.password);
    pack.setInt("ap_channel", cfg.channel);
    pack.setInt("ap_max_conn", cfg.maxConnections);
    pack.setInt("ap_hidden", cfg.hidden ? 1 : 0);
    pack.setStr("ap_ip", cfg.ip);
    pack.setStr("ap_gateway", cfg.gateway);
    pack.setStr("ap_subnet", cfg.subnet);
    m_sim->addAssoc("wifi", "ap", "WiFi AP " + cfg.ssid);
    sendApCmd(pack);

    // 本地立即视为已启动(模拟器随后通过 station_info 包回传站点接入/断开)
    JZWifiDriver::onApStarted(m_apWifi);
}

void JZWifiDriverSimulator::apStop(JZWifiAp *w)
{
    JZSimPack pack;
    pack.cmd = SIM_WIFI_AP_STOP;
    m_sim->removeAssoc("wifi", "ap");
    sendApCmd(pack);

    m_apStations.clear();
    m_apWifi = JZObjectPointer();
    JZWifiDriver::onApStopped(w->guardPointer());
}

bool JZWifiDriverSimulator::isApRunning(JZWifiAp *w)
{
    (void)w;
    return true;
}

int JZWifiDriverSimulator::apStationCount(JZWifiAp *w)
{
    (void)w;
    // 返回当前连接到本 AP 的站点集合大小(由模拟器回传的完整站点列表维护)
    return (int)m_apStations.size();
}

// ==================== Scan (扫描) ====================
// 扫描为本地行为: 直接返回固定结果 jz_simulator_ap(不发网络包)。
// 注: 忽略 JZScanConfig 标记(模拟环境无真实射频, 无"低影响"概念)。
void JZWifiDriverSimulator::scanStart(JZWifiScanner *s, const JZScanConfig &cfg)
{
    (void)cfg;

    // 扫描结果由服务端配置(JZSimServer::config()["wifi"]["aps"])决定外围 AP 列表
    const Json::Value& wifiCfg = JZSimServer::instance()->config()["wifi"];
    const Json::Value& aps = (wifiCfg.isObject() && wifiCfg.isMember("aps")) ? wifiCfg["aps"] : Json::Value(Json::arrayValue);

    JZWifiScanResult r;
    if (aps.isArray())
    {
        for (auto it = aps.begin(); it != aps.end(); ++it)
        {
            const Json::Value& a = *it;
            JZWifiScanResult::ApInfo ap;
            ap.ssid  = a.isMember("ssid")  ? a["ssid"].asString()  : std::string();
            ap.bssid = a.isMember("bssid") ? a["bssid"].asString() : std::string();
            ap.rssi  = a.isMember("rssi")  ? a["rssi"].asInt()      : -50;
            ap.channel = a.isMember("channel") ? a["channel"].asInt() : 6;
            ap.authMode = a.isMember("auth") ? a["auth"].asInt() : 0;  // 默认 OPEN
            r.apList.push_back(ap);
        }
    }
    JZWifiDriver::onScanFinished(s->guardPointer(), r);
}

void JZWifiDriverSimulator::scanStop(JZWifiScanner *s)
{
    (void)s;
}

bool JZWifiDriverSimulator::isScanning(JZWifiScanner *s)
{
    (void)s;
    return false;
}

// ==================== 报文分发 (下行: 模拟器 -> Driver) ====================
void JZWifiDriverSimulator::onRecv(const JZSimPack &pack)
{
    // 外围 AP 清单变更事件: server 对 wifi.aps 做 diff 后发出(增/删/改)。
    // scan 结果由 scanStart 实时读 config(), 无需维护缓存; 仅当变更使当前 station
    // 凭证失效时断开 station(如密码/ssid 改变、AP 被删除)。rssi 改变不影响连接。
    if (pack.cmd == SIM_WIFI_AP_ADDED ||
        pack.cmd == SIM_WIFI_AP_UPDATED ||
        pack.cmd == SIM_WIFI_AP_REMOVED)
    {
        jzDebug() << "JZWifiDriverSimulator: wifi ap event" << (int)pack.cmd;
        checkStationCredential();
        return;
    }

    // station 连接结果: 由模拟器回发, 决定本次连接是否真正成功(不再本地默认成功)。
    if (pack.cmd == SIM_WIFI_STATION_RESULT)
    {
        auto ok = pack.getBool("ok");
        if (!ok.isOk() || !ok.value() || !m_staWifi)
        {
            if (m_staWifi)
                JZWifiDriver::onStationError(m_staWifi, -1, "connect failed");
            m_staWifi = JZObjectPointer();
            m_staSsid.clear();
            m_staPassword.clear();
            return;
        }
        // 成功: 以回包中的 ssid/password 为准提交为当前已连接 station。
        m_staSsid = pack.getStr("ssid").valueOr("");
        m_staPassword = pack.getStr("password").valueOr("");
        std::string bssid = pack.getStr("bssid").valueOr("");
        if (bssid.empty()) bssid = "00:11:22:33:44:55";
        JZWifiDriver::onStationConnected(m_staWifi, localIp().c_str(), bssid.c_str());
        return;
    }

    if (!m_apWifi)
        return;

    // 使用成员 API(统一 JZResult)读取下行 station_info 中的完整站点列表
    auto macs = pack.getStrList("stations_macs");
    auto ips  = pack.getStrList("stations_ips");
    std::vector<JZWifiStationInfo> stations;
    if (macs.isOk() && ips.isOk())
    {
        const auto &ml = macs.value();
        const auto &il = ips.value();
        stations.reserve(ml.size());
        for (size_t i = 0; i < ml.size(); i++)
        {
            JZWifiStationInfo s;
            s.mac = ml[i];
            s.ip = (i < il.size()) ? il[i] : std::string();
            stations.push_back(s);
        }
    }

    // 与本地已知集合做差分, 触发相应的接入/断开事件(本地对比, 不发网络包)。
    std::map<std::string, std::string> next;
    for (auto &s : stations)
        next[s.mac] = s.ip;

    for (auto &s : stations)   // 新接入的站点
    {
        if (m_apStations.find(s.mac) == m_apStations.end())
            JZWifiDriver::onApStationConnected(m_apWifi, s.mac.c_str(), s.ip.c_str());
    }
    for (auto &kv : m_apStations)   // 已断开的站点
    {
        if (next.find(kv.first) == next.end())
            JZWifiDriver::onApStationDisconnected(m_apWifi, kv.first.c_str(), kv.second.c_str());
    }
    m_apStations = next;
}

// 配置 wifi.aps 变更后调用: 当前 station 凭证在新配置中若已无匹配 AP, 断开 station,
// 让上层按真实掉线处理(触发重连)。
void JZWifiDriverSimulator::checkStationCredential()
{
    if (!m_staWifi || m_staSsid.empty())
        return;
    JZWifiConfig staCfg;
    staCfg.ssid = m_staSsid;
    staCfg.password = m_staPassword;
    std::string bssid = matchApBssid(staCfg);
    if (bssid.empty())
    {
        jzDebug() << "JZWifiDriverSimulator: station credential no longer matches config -> disconnect";
        auto* w = static_cast<JZWifiStation*>(m_staWifi.object());
        stationDisconnect(w);
    }
}
