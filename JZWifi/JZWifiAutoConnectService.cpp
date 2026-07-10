#include "JZWifiAutoConnectService.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZWifiAutoConnectService, JZObject)

JZWifiAutoConnectService::JZWifiAutoConnectService(JZObject *parent)
    : JZObject(parent),
      sigConnecting(this), sigConnected(this), sigError(this),
      m_scanner(new JZWifiScanner(this)),
      m_station(new JZWifiStation(this))
{
    connect(&m_scanner->sigFinished, this, &JZWifiAutoConnectService::onScanFinished);
    connect(&m_station->sigConnected, this, &JZWifiAutoConnectService::onStationConnected);
    connect(&m_station->sigDisconnected, this, &JZWifiAutoConnectService::onStationDisconnected);
    connect(&m_station->sigError, this, &JZWifiAutoConnectService::onStationError);
}

JZWifiAutoConnectService::~JZWifiAutoConnectService()
{
    stop();
}

void JZWifiAutoConnectService::setCandidates(const std::vector<JZWifiCredential> &list) { m_candidates = list; }
std::vector<JZWifiCredential> JZWifiAutoConnectService::candidates() const { return m_candidates; }

void JZWifiAutoConnectService::setScanIntervalMs(int ms)
{
    m_intervalMs = ms > 0 ? ms : 1000;
    m_effectiveInterval = m_intervalMs;
    if (m_active) armTimer();
}
int JZWifiAutoConnectService::scanIntervalMs() const { return m_intervalMs; }

void JZWifiAutoConnectService::setSwitchHysteresis(int dBm) { m_switchHysteresis = dBm; }
int JZWifiAutoConnectService::switchHysteresis() const { return m_switchHysteresis; }

void JZWifiAutoConnectService::setGoodRssiThreshold(int dBm) { m_goodRssiThreshold = dBm; }
int JZWifiAutoConnectService::goodRssiThreshold() const { return m_goodRssiThreshold; }

void JZWifiAutoConnectService::setFailBlockBaseMs(int ms) { m_failBlockBaseMs = ms > 0 ? ms : 1000; }
int JZWifiAutoConnectService::failBlockBaseMs() const { return m_failBlockBaseMs; }
void JZWifiAutoConnectService::setFailBlockMaxMs(int ms) { m_failBlockMaxMs = ms > 0 ? ms : m_failBlockBaseMs; }
int JZWifiAutoConnectService::failBlockMaxMs() const { return m_failBlockMaxMs; }

void JZWifiAutoConnectService::clearFailBlacklist(const std::string &ssid)
{
    if (ssid.empty()) m_failBlacklist.clear();
    else m_failBlacklist.erase(ssid);
}

void JZWifiAutoConnectService::armTimer()
{
    if (m_timerId != INVAILD_TIMER_ID) killTimer(m_timerId);
    m_timerId = startTimer(m_effectiveInterval);
}

void JZWifiAutoConnectService::start()
{
    if (m_active) return;
    if (m_candidates.empty()) {
        jzWarning() << "JZWifiAutoConnectService: 候选列表为空";
        return;
    }
    m_active = true;
    m_effectiveInterval = m_intervalMs;
    armTimer();                          // 按基础间隔启动周期定时器
    m_scanner->start();                  // 立即先扫描一次
}

void JZWifiAutoConnectService::stop()
{
    if (!m_active) return;
    m_active = false;
    if (m_timerId != INVAILD_TIMER_ID) { killTimer(m_timerId); m_timerId = INVAILD_TIMER_ID; }
    m_scanner->stop();
    m_effectiveInterval = m_intervalMs;
    m_failBlacklist.clear();
}

bool JZWifiAutoConnectService::isActive() const { return m_active; }
std::string JZWifiAutoConnectService::connectedSsid() const { return m_connectedSsid; }
std::vector<JZWifiScanResult> JZWifiAutoConnectService::lastScanResults() const { return m_lastResults; }

void JZWifiAutoConnectService::timerEvent(JZTimerEvent *event)
{
    if (event->id() != m_timerId) return;
    if (m_connecting) return;                              // 正在连接, 不抢扫
    // 已连接且信号良好: 跳过本次周期扫描以省电
    if (!m_connectedSsid.empty() && m_connectedRssi >= m_goodRssiThreshold)
        return;
    if (!m_scanner->isActive())
        m_scanner->start();
}

void JZWifiAutoConnectService::onScanFinished()
{
    m_lastResults = m_scanner->results();
    evaluateAndConnect();

    // 退避策略: 每次扫描若仍未连上, 间隔翻倍(上限 m_maxBackoffMs); 一旦连上即复位为基础间隔
    if (m_connecting) return;                              // 正在连接, 维持当前间隔
    if (!m_connectedSsid.empty()) {
        m_effectiveInterval = m_intervalMs;
    } else {
        int next = m_effectiveInterval * 2;
        m_effectiveInterval = (next > m_maxBackoffMs) ? m_maxBackoffMs : next;
    }
    armTimer();
}

void JZWifiAutoConnectService::onStationConnected()
{
    m_connecting = false;
    m_connectedSsid = m_pendingSsid;
    m_connectedRssi = m_pendingRssi;
    m_failBlacklist.erase(m_connectedSsid);     // 连上即清除该 ssid 的失败黑名单
    sigConnected.emit(m_connectedSsid);
}

void JZWifiAutoConnectService::onStationDisconnected()
{
    m_connecting = false;
    if (!m_connectedSsid.empty()) {
        m_connectedSsid.clear();
        // 已丢失连接, 重新评估并尝试连接其它可见候选
        if (m_active && !m_scanner->isActive())
            m_scanner->start();
    }
}

void JZWifiAutoConnectService::onStationError(int code, const std::string &msg)
{
    m_connecting = false;
    sigError.emit(code, msg);

    // 将本次尝试的 ssid 加入/延长失败黑名单(指数退避: base * 2^(n-1), 封顶 max)
    if (!m_pendingSsid.empty()) {
        JZWifiFailInfo &info = m_failBlacklist[m_pendingSsid];
        info.failCount++;
        int64_t blockMs = m_failBlockBaseMs;
        for (int i = 1; i < info.failCount; i++) {
            int64_t next = blockMs * 2;
            blockMs = (next > m_failBlockMaxMs) ? m_failBlockMaxMs : next;
        }
        info.blockUntil = JZPlatform::getTickCountMs() + blockMs;
    }

    // 连接失败, 稍后重新扫描评估(黑名单内的会被跳过, 转试其它候选)
    if (m_active && !m_scanner->isActive())
        m_scanner->start();
}

void JZWifiAutoConnectService::evaluateAndConnect()
{
    // 在可见候选中挑选信号最强的 (rssi 为负, 越大越强)
    const JZWifiCredential *best = nullptr;
    int bestRssi = -127;
    for (const auto &r : m_lastResults) {
        for (const auto &c : m_candidates) {
            if (c.ssid != r.ssid) continue;
            // 跳过仍在冷却中的失败黑名单 ssid
            auto it = m_failBlacklist.find(c.ssid);
            if (it != m_failBlacklist.end() && it->second.blockUntil > JZPlatform::getTickCountMs())
                continue;
            if (r.rssi > bestRssi) { bestRssi = r.rssi; best = &c; }
        }
    }
    if (!best) return;                                   // 当前无可见候选

    if (m_connectedSsid == best->ssid) return;           // 已是最强目标

    // 已连接其它候选时, 仅当新目标明显更强才切换 (避免抖动)
    if (!m_connectedSsid.empty() && bestRssi <= m_connectedRssi + m_switchHysteresis)
        return;

    if (m_connecting) return;

    m_connecting = true;
    m_pendingSsid = best->ssid;
    m_pendingRssi = bestRssi;
    sigConnecting.emit(best->ssid);

    JZWifiConfig cfg;
    cfg.ssid = best->ssid;
    cfg.password = best->password;
    m_station->connect(cfg);
}
