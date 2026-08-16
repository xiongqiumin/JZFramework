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
    if (m_timerId != INVALID_TIMER_ID) 
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
    if (m_timerId != INVALID_TIMER_ID) 
    { 
        killTimer(m_timerId); 
        m_timerId = INVALID_TIMER_ID; 
    }
    stopRssiMonitor();

    m_scanner->stop();

    m_effectiveInterval = m_config.scanIntervalMs;
    m_failBlacklist.clear();    
}

void JZWifiAutoConnectService::disconnectFromAp()
{
    m_station->disconnectFromAp();
}

bool JZWifiAutoConnectService::isActive() const 
{ 
    return m_active; 
}

bool JZWifiAutoConnectService::isConnected() const
{
    return !m_connectedSsid.empty();
}

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
    if (event->id() == m_rssiTimerId) {
        onRssiPoll();
        return;
    }
    if (event->id() != m_timerId) 
        return;

    // 正在连接, 不抢扫
    if (isStationConnecting()) 
        return;                              
    
    // 已连接时:
    //  - 信号很差(< poorRssiThreshold): 立即重扫以寻找更优 AP 漫游
    //  - 信号尚可但低于漫游扫描阈值 roamScanRssiThreshold: 按 roamScanIntervalMs 节流周期扫描,
    //    以发现更强的同名 AP(同 SSID 漫游); 高于该阈值则信号很好, 无需找更优 AP, 跳过扫描省电
    //  - 未连接: 正常周期扫描
    bool doRoamScan = false;
    if (!m_connectedSsid.empty()) {
        int64_t now = JZPlatform::getTickCountMs();
        if (m_connectedRssi < m_config.poorRssiThreshold) {
            // 信号已很差: 尽快重扫寻找更优 AP 漫游, 仍受 roamScanIntervalMs 节流避免反复扫描
            if (m_config.roamScanIntervalMs > 0 && now - m_lastRoamScanMs < m_config.roamScanIntervalMs)
                return;                           // 未到扫描周期, 继续省电
            m_lastRoamScanMs = now;
        } else if (m_config.roamScanIntervalMs > 0 && m_connectedRssi < m_config.roamScanRssiThreshold) {
            if (now - m_lastRoamScanMs < m_config.roamScanIntervalMs)
                return;                           // 未到漫游扫描周期, 继续省电
            m_lastRoamScanMs = now;
            doRoamScan = true;                    // 低影响漫游扫描: 减小对已建链路的打扰
        } else {
            return;                               // 信号很好或漫游扫描未启用, 跳过扫描省电
        }
    }

    if (!m_scanner->isActive()) 
    {
        jzDebug() << "connect rssi" << m_connectedRssi << ", roam threshold" << m_config.roamScanRssiThreshold
                  << ", poor threshold" << m_config.poorRssiThreshold << ", rescan";
        if (doRoamScan)
            m_scanner->startRoamScan(m_connectedSsid, m_config.roamScanAllChannels);
        else
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
    m_connectedBssid = m_station->bssid();      // 记录当前连接的 AP BSSID, 用于同 SSID 漫游判断
    m_pendingIsRoam = false;
    m_lastRoamScanMs = JZPlatform::getTickCountMs();

    m_failBlacklist.erase(m_connectedSsid);     // 连上即清除该 ssid 的失败黑名单
    startRssiMonitor();                          // 启动 RSSI 轮询, 实时跟踪信号变化
    sigConnected.emit(m_connectedSsid);
}

void JZWifiAutoConnectService::onStationDisconnected()
{    
    jzDebug() << "JZWifiAutoConnectService::onStationDisconnected";
    if (!m_connectedSsid.empty()) {
        stopRssiMonitor();
        sigDisconnected.emit();

        m_connectedSsid.clear();
        m_connectedRssi = -127;
        m_connectedBssid.clear();
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

    // 漫游(同 SSID 换更强 AP)失败: 不拉黑整个 SSID(否则会阻断其它同名 AP 回退),
    // 改为临时屏蔽该 BSSID, 并去掉钉选重试连任意同名 AP
    if (m_pendingIsRoam) {
        m_pendingIsRoam = false;
        if (!m_pendingBssid.empty()) {
            int64_t blockMs = m_config.failBlockBaseMs;
            m_bssidBlock[m_pendingBssid] = JZPlatform::getTickCountMs() + blockMs;
        }
        m_pendingBssid.clear();
        if (m_active && !m_scanner->isActive()) {
            m_scanner->start();
            sigScanning.emit();
        }
        return;
    }

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

void JZWifiAutoConnectService::startRssiMonitor()
{
    if (m_rssiTimerId != INVALID_TIMER_ID)
        killTimer(m_rssiTimerId);
    m_rssiTimerId = startTimer(m_config.rssiIntervalMs > 0 ? m_config.rssiIntervalMs : 15000);
}

void JZWifiAutoConnectService::stopRssiMonitor()
{
    if (m_rssiTimerId != INVALID_TIMER_ID) {
        killTimer(m_rssiTimerId);
        m_rssiTimerId = INVALID_TIMER_ID;
    }
}

void JZWifiAutoConnectService::onRssiPoll()
{
    if (m_connectedSsid.empty())
        return;
    int cur = m_station->currentRssi();
    if (cur != m_connectedRssi) {
        m_connectedRssi = cur;
        jzDebug() << "connected rssi change to" << cur;
        sigRssiChanged.emit(cur);
    }
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
    cfg.bssid = m_pendingBssid;     // 钉选目标 AP, 实现同 SSID 漫游到指定物理 AP
    m_station->connectToAp(cfg);
    m_connectTimer->stop();
}

void JZWifiAutoConnectService::evaluateAndConnect()
{
    // 在可见候选 AP 中挑选信号最强的 (rssi 为负, 越大越强), 同时记住其 bssid
    const JZWifiCredential *best = nullptr;
    const JZWifiScanResult::ApInfo *bestAp = nullptr;
    int bestRssi = -127;
    int64_t now = JZPlatform::getTickCountMs();
    for (const auto &ap : m_lastResults.apList) {
        for (const auto &c : m_config.candidates) {
            if (c.ssid != ap.ssid) continue;
            // 跳过仍在冷却中的失败黑名单 ssid
            auto it = m_failBlacklist.find(c.ssid);
            if (it != m_failBlacklist.end() && it->second.blockUntil > now)
                continue;
            // 跳过漫游失败被临时屏蔽的 bssid
            auto bit = m_bssidBlock.find(ap.bssid);
            if (bit != m_bssidBlock.end() && bit->second > now)
                continue;
            if (ap.rssi > bestRssi) { bestRssi = ap.rssi; best = &c; bestAp = &ap; }
        }
    }
    if (!best || !bestAp) 
        return;                                   // 当前无可见候选

    // 已连接同一 SSID 且同一 BSSID -> 无需任何动作
    if (m_connectedSsid == best->ssid && m_connectedBssid == bestAp->bssid)
        return;

    // 已连接同一 SSID 但不同(且更强) BSSID -> 主动漫游切换(同 SSID 内换更强 AP)
    if (m_connectedSsid == best->ssid) {
        if (bestRssi <= m_connectedRssi + m_config.switchHysteresis)
            return;                               // 不够强, 避免抖动
        jzDebug() << "roam same ssid to stronger bssid" << bestAp->bssid
                  << ", rssi" << bestRssi << " (cur " << m_connectedBssid << " " << m_connectedRssi << ")";
        JZWifiConfig cfg;
        cfg.ssid = best->ssid;
        cfg.password = best->password;
        cfg.bssid = bestAp->bssid;     // 钉选更强 AP, 实现同 SSID 漫游切换
        m_pendingSsid = *best;
        m_pendingBssid = bestAp->bssid;
        m_pendingRssi = bestRssi;
        m_pendingIsRoam = true;
        m_station->switchToAp(cfg);    // 不停回 None, 不延迟 500ms, 直接切换(中间断开被吞掉)
        return;
    }

    // 不同 SSID: 已连接时仅当新目标明显更强才切换 (避免抖动)
    if (!m_connectedSsid.empty() && bestRssi <= m_connectedRssi + m_config.switchHysteresis)
        return;

    jzDebug() << "connect ssid" << m_connectedSsid << ", rssi" << m_connectedRssi << ", best ssid" << 
        best->ssid << ", rssi" << bestRssi;
    m_station->disconnectFromAp();
    
    m_pendingSsid = *best;        
    m_pendingBssid = bestAp->bssid;
    m_pendingRssi = bestRssi;
    m_pendingIsRoam = false;
    m_connectTimer->start(500);    
}
