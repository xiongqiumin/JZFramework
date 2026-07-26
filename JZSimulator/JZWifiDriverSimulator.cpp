#include "JZWifiDriverSimulator.h"
#include "JZDebug.h"
#include "JZNetworkInterface.h"
#include "JZSignal.h"
#include "JZApplication.h"

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
    JZSimPack::ensureRegistered();

    m_client = new JZNetClient();
    connect(&m_client->sigPacketReceive, [this](JZNetPackPtr p) { onPacket(p); });
    m_client->connectToHost(ip, port);
    if (jzApp)
        m_client->waitForConnected(500);
}

JZWifiDriverSimulator::~JZWifiDriverSimulator()
{
    if (m_client)
    {
        delete m_client;
        m_client = nullptr;
    }
}

bool JZWifiDriverSimulator::isWifiAvailable()
{
    return true;
}

void JZWifiDriverSimulator::sendApCmd(const JZSimPack &pack)
{
    if (!m_client || !m_client->isConnect())
        return;
    m_client->sendPacket(&pack);
}

// ==================== Station (连接) ====================
// Station 连接为本地行为: 仅做凭证校验, 成功即视为已连接(不发网络包)。
void JZWifiDriverSimulator::stationConnect(JZWifiStation *w, const JZWifiConfig &cfg)
{
    // 连接规则: 仅接受指定 AP 的凭证, 其它一律失败
    bool ok = (cfg.ssid == kApSsid) && (cfg.password == kApPassword);
    if (!ok)
    {
        JZWifiDriver::onStationError(w->guardPointer(), -1, "invalid ssid or password");
        return;
    }

    // 成功: 本地视为已连接(不发网络包)
    m_staWifi = w->guardPointer();
    std::string staBssid = cfg.bssid.empty() ? std::string("00:11:22:33:44:55") : cfg.bssid;
    JZWifiDriver::onStationConnected(m_staWifi, localIp().c_str(), staBssid.c_str());
}

void JZWifiDriverSimulator::stationDisconnect(JZWifiStation *w)
{
    m_staWifi = JZObjectPointer();
    JZWifiDriver::onStationDisconnected(w->guardPointer());
}

void JZWifiDriverSimulator::stationSwitch(JZWifiStation *w, const JZWifiConfig &cfg)
{
    // 模拟: 直接视为已切到新 AP(本地校验, 不发网络包), 不发中间断开事件
    bool ok = (cfg.ssid == kApSsid) && (cfg.password == kApPassword);
    if (!ok) {
        // 切换失败: 发送断线事件(由上层按真实掉线处理, 触发全链路重连)
        m_staWifi = JZObjectPointer();
        JZWifiDriver::onStationDisconnected(w->guardPointer());
        return;
    }
    std::string staBssid = cfg.bssid.empty() ? std::string("00:11:22:33:44:55") : cfg.bssid;
    JZWifiDriver::onStationConnected(w->guardPointer(), localIp().c_str(), staBssid.c_str());
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
    sendApCmd(pack);

    // 本地立即视为已启动(模拟器随后通过 station_info 包回传站点接入/断开)
    JZWifiDriver::onApStarted(m_apWifi);
}

void JZWifiDriverSimulator::apStop(JZWifiAp *w)
{
    JZSimPack pack;
    pack.cmd = SIM_WIFI_AP_STOP;
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

    JZWifiScanResult r;
    JZWifiScanResult::ApInfo ap;
    ap.ssid = "jz_simulator_ap";
    ap.rssi = -50;
    ap.bssid = "00:11:22:33:44:55";
    ap.channel = 6;
    ap.authMode = 0;   // WIFI_AUTH_OPEN
    r.apList.push_back(ap);
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
void JZWifiDriverSimulator::onPacket(JZNetPackPtr pack)
{
    // 下行包: 模拟器回传"当前连接到本 AP 的所有站点"完整信息
    auto *sp = dynamic_cast<JZSimPack *>(pack.get());
    if (!sp)
        return;

    if (!m_apWifi)
        return;

    // 使用成员 API(统一 JZResult)读取下行 station_info 中的完整站点列表
    auto macs = sp->getStrList("stations_macs");
    auto ips  = sp->getStrList("stations_ips");
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
