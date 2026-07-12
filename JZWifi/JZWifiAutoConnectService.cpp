#include "JZWifiAutoConnectService.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZWifiAutoConnectService, JZObject)

JZWifiAutoConnectService::JZWifiAutoConnectService(JZObject *parent)
    : JZObject(parent),
      sigScanning(this), sigConnecting(this), sigConnected(this), sigDisconnected(this),
      sigError(this), sigRssiChanged(this),
      m_scanner(new JZWifiScanner(this)),
      m_station(new JZWifiStation(this))
{
    connect(&m_scanner->sigFinished, this, &JZWifiAutoConnectService::onScanFinished);
    connect(&m_scanner->sigError, this, &JZWifiAutoConnectService::onScanError);

    connect(&m_station->sigConnected, this, &JZWifiAutoConnectService::onStationConnected);
    connect(&m_station->sigDisconnected, this, &JZWifiAutoConnectService::onStationDisconnected);
    connect(&m_station->sigError, this, &JZWifiAutoConnectService::onStationError);
    connect(&m_station->sigRssiChanged, this, &JZWifiAutoConnectService::onStationRssiChanged);

    m_connectTimer = new JZTimer(this);
    connect(&m_connectTimer->sigTimeout, this, &JZWifiAutoConnectService::onConnectTimer);
}

JZWifiAutoConnectService::~JZWifiAutoConnectService()
{
    stop();
}

void JZWifiAutoConnectService::setConfig(const JZWifiAutoConnectServiceConfig &config)
{
    m_config = config;
    if (m_config.scanIntervalMs <= 0)
        m_config.scanIntervalMs = 1000;
    if (m_config.failBlockBaseMs <= 0)
        m_config.failBlockBaseMs = 1000;
    if (m_config.failBlockMaxMs <= 0)
        m_config.failBlockMaxMs = m_config.failBlockBaseMs;

    m_effectiveInterval = m_config.scanIntervalMs;
    if (m_active) armTimer();
}

JZWifiAutoConnectServiceConfig JZWifiAutoConnectService::config() const { return m_config; }

void JZWifiAutoConnectService::clearFailBlacklist(const std::string &ssid)
{
    if (ssid.empty()) m_failBlacklist.clear();
    else m_failBlacklist.erase(ssid);
}

void JZWifiAutoConnectService::armTimer()
{
    if (m_timerId != INVAILD_TIMER_ID) 
        killTimer(m_timerId);

    m_timerId = startTimer(m_effectiveInterval);
}

void JZWifiAutoConnectService::start()
{
    if (m_active) return;
    if (m_config.candidates.empty()) {
        jzWarning() << "JZWifiAutoConnectService: 候选列表为空";
        return;
    }
    m_active = true;
    m_effectiveInterval = m_config.scanIntervalMs;
    armTimer();                          // 按基础间隔启动周期定时器
    m_scanner->start();                  // 立即先扫描一次
    sigScanning.emit();
}

void JZWifiAutoConnectService::stop()
{
    if (!m_active) 
        return;

    m_active = false;
    if (m_timerId != INVAILD_TIMER_ID) 
    { 
        killTimer(m_timerId); 
        m_timerId = INVAILD_TIMER_ID; 
    }

    m_scanner->stop();
    m_station->disconnectFromAp();

    m_effectiveInterval = m_config.scanIntervalMs;
    m_failBlacklist.clear();    
}

bool JZWifiAutoConnectService::isActive() const { return m_active; }
JZWifiConnectInfo JZWifiAutoConnectService::connectedInfo() const 
{ 
    JZWifiConnectInfo info;
    info.ssid = m_connectedSsid;
    info.rssi = m_connectedRssi;
    return info; 
}
JZWifiScanResult JZWifiAutoConnectService::lastScanResults() const { return m_lastResults; }
bool JZWifiAutoConnectService::isStationConnecting()
{
    return m_station->state() == JZWifiStation::Connecting;
}

void JZWifiAutoConnectService::timerEvent(JZTimerEvent *event)
{
    if (event->id() != m_timerId) 
        return;

    // 正在连接, 不抢扫
    if (isStationConnecting()) 
        return;                              
    
        // 已连接且信号良好: 跳过本次周期扫描以省电
    if (!m_connectedSsid.empty() && m_connectedRssi >= m_config.goodRssiThreshold)
        return;
    
    if (!m_scanner->isActive()) {
        m_scanner->start();
        sigScanning.emit();
    }
}

void JZWifiAutoConnectService::onScanFinished(const JZWifiScanResult &result)
{
    jzDebug() << "JZWifiAutoConnectService::onScanFinished";

    m_lastResults = result;
    evaluateAndConnect();

    // 退避策略: 每次扫描若仍未连上, 间隔翻倍(上限 maxBackoffMs); 一旦连上即复位为基础间隔
    if (isStationConnecting()) 
        return;                              // 正在连接, 维持当前间隔

    if (!m_connectedSsid.empty()) {
        m_effectiveInterval = m_config.scanIntervalMs;
    } else {
        int next = m_effectiveInterval * 2;
        m_effectiveInterval = (next > m_config.maxBackoffMs) ? m_config.maxBackoffMs : next;
    }
    armTimer();
}

void JZWifiAutoConnectService::onScanError(int code, const std::string &msg)
{
    jzDebug() << "scan error code" << code << ", message" << msg;
}

void JZWifiAutoConnectService::onStationConnected()
{   
    jzDebug() << "JZWifiAutoConnectService::onStationConnected";

    m_connectedSsid = m_pendingSsid.ssid;    
    m_connectedRssi = m_pendingRssi;

    m_failBlacklist.erase(m_connectedSsid);     // 连上即清除该 ssid 的失败黑名单
    m_station->startRssiMonitor(m_config.rssiIntervalMs);  // 启动 RSSI 轮询, 实时跟踪信号变化
    sigConnected.emit(m_connectedSsid);
}

void JZWifiAutoConnectService::onStationDisconnected()
{    
    jzDebug() << "JZWifiAutoConnectService::onStationDisconnected";
    if (!m_connectedSsid.empty()) {
        m_station->stopRssiMonitor();
        sigDisconnected.emit();

        m_connectedSsid.clear();
        m_connectedRssi = -127;
        // 已丢失连接, 重新评估并尝试连接其它可见候选
        if (m_active && !m_scanner->isActive()) {
            m_scanner->start();
            sigScanning.emit();
        }
    }
}

void JZWifiAutoConnectService::onStationError(int code, const std::string &msg)
{    
    sigError.emit(code, msg);

    // 将本次尝试的 ssid 加入/延长失败黑名单(指数退避: base * 2^(n-1), 封顶 max)
    if (!m_pendingSsid.ssid.empty()) {
        WifiFailInfo &info = m_failBlacklist[m_pendingSsid.ssid];
        info.failCount++;
        int64_t blockMs = m_config.failBlockBaseMs;
        for (int i = 1; i < info.failCount; i++) {
            int64_t next = blockMs * 2;
            blockMs = (next > m_config.failBlockMaxMs) ? m_config.failBlockMaxMs : next;
        }
        info.blockUntil = JZPlatform::getTickCountMs() + blockMs;
    }

    // 连接失败, 稍后重新扫描评估(黑名单内的会被跳过, 转试其它候选)
    if (m_active && !m_scanner->isActive()) {
        m_scanner->start();
        sigScanning.emit();
    }
}

void JZWifiAutoConnectService::onStationRssiChanged(int rssi)
{
    m_connectedRssi = rssi;
    sigRssiChanged.emit(rssi);
}

void JZWifiAutoConnectService::onConnectTimer()
{
    if(m_pendingSsid.ssid.empty())
    {
        m_connectTimer->stop();
        return;
    }

    if(m_station->state() != JZWifiStation::None)
        return;

    sigConnecting.emit(m_pendingSsid.ssid);

    JZWifiConfig cfg;
    cfg.ssid = m_pendingSsid.ssid;
    cfg.password = m_pendingSsid.password;
    m_station->connectToAp(cfg);
    m_connectTimer->stop();
}

void JZWifiAutoConnectService::evaluateAndConnect()
{
    // 在可见候选中挑选信号最强的 (rssi 为负, 越大越强)
    const JZWifiCredential *best = nullptr;
    int bestRssi = -127;
    for (const auto &ap : m_lastResults.apList) {
        for (const auto &c : m_config.candidates) {
            if (c.ssid != ap.ssid) continue;
            // 跳过仍在冷却中的失败黑名单 ssid
            auto it = m_failBlacklist.find(c.ssid);
            if (it != m_failBlacklist.end() && it->second.blockUntil > JZPlatform::getTickCountMs())
                continue;
            if (ap.rssi > bestRssi) { bestRssi = ap.rssi; best = &c; }
        }
    }
    if (!best) 
        return;                                   // 当前无可见候选

    if (m_connectedSsid == best->ssid) 
        return;           // 已是最强目标

    // 已连接其它候选时, 仅当新目标明显更强才切换 (避免抖动)
    if (!m_connectedSsid.empty() && bestRssi <= m_connectedRssi + m_config.switchHysteresis)
        return;

    m_station->disconnectFromAp();
    
    m_pendingSsid = *best;        
    m_pendingRssi = bestRssi;
    m_connectTimer->start(500);    
}
