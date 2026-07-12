#ifndef JZ_WIFI_AUTO_CONNECT_SERVICE_H_
#define JZ_WIFI_AUTO_CONNECT_SERVICE_H_

#include "JZObject.h"
#include "JZEvent.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"
#include "JZWifiStation.h"
#include "JZWifiScanner.h"
#include "JZPlatform.h"
#include <string>
#include <vector>
#include <unordered_map>

// 自动连接候选: ssid + 密码
struct JZWifiCredential
{
    std::string ssid;
    std::string password;
};

// 失败黑名单条目: 记录某 ssid 连续连接失败次数与冷却到期时间戳
struct JZWifiFailInfo
{
    int failCount = 0;        // 连续失败次数
    int64_t blockUntil = 0;   // 冷却到期时间(currentMSecsSinceEpoch, 毫秒)
};

// JZWifiAutoConnectService 自动连接服务。
// 输入一组候选网络(序列), 按设定间隔定期扫描, 在可见候选中选取信号强度(rssi)最大
// 的网络并连接; 当已连接网络丢失或发现更强候选时自动切换。
class JZWifiAutoConnectService : public JZObject
{
    JZ_OBJECT_DECLARE(JZWifiAutoConnectService)

public:
    JZWifiAutoConnectService(JZObject *parent = nullptr);
    ~JZWifiAutoConnectService() override;

    // 候选网络序列
    void setCandidates(const std::vector<JZWifiCredential> &list);
    std::vector<JZWifiCredential> candidates() const;

    // 定期扫描间隔(毫秒)
    void setScanIntervalMs(int ms);
    int scanIntervalMs() const;

    // 切换迟滞(dBm): 仅当新候选比当前连接强出该值才切换, 避免抖动
    void setSwitchHysteresis(int dBm);
    int switchHysteresis() const;

    // 信号良好阈值(dBm): 已连接且 rssi >= 该值时跳过周期扫描以省电
    // (默认 -60, 约 "good" 档; -67 为常见良好边界, 越大越省电)
    void setGoodRssiThreshold(int dBm);
    int goodRssiThreshold() const;

    // 失败黑名单: 连接失败的 ssid 进入冷却, 冷却时间按 base * 2^(n-1) 指数退避(封顶 max)
    void setFailBlockBaseMs(int ms);
    int failBlockBaseMs() const;
    void setFailBlockMaxMs(int ms);
    int failBlockMaxMs() const;
    void clearFailBlacklist(const std::string &ssid = std::string());  // 清空(不传则全部)

    void start();
    void stop();
    bool isActive() const;

    // 当前已连接 ssid (空表示未连接)
    std::string connectedSsid() const;

    // 最近一次扫描到的候选网络及其强度
    std::vector<JZWifiScanResult> lastScanResults() const;

jz_signals:
    JZSignal<const std::string&> sigConnecting;   // 正在连接某 ssid
    JZSignal<const std::string&> sigConnected;    // 已连接到某 ssid
    JZSignal<> sigDisconnected;  
    JZSignal<int, const std::string&> sigError;

protected:
    void timerEvent(JZTimerEvent *event) override;

    void onScanFinished();
    void onScanError(int code, const std::string &msg);

    void onStationConnected();
    void onStationDisconnected();
    void onStationError(int code, const std::string &msg);

    void evaluateAndConnect();
    void armTimer();                              // 按当前 m_effectiveInterval 重启周期定时器
    void disconnectFromAp();

    JZWifiScanner *m_scanner;
    JZWifiStation *m_station;

    std::vector<JZWifiCredential> m_candidates;
    bool m_active = false;
    int m_intervalMs = 15000;                     // 基础扫描间隔
    int m_effectiveInterval = 15000;              // 当前生效间隔(退避时翻倍)
    int m_maxBackoffMs = 60000;                   // 退避上限
    int m_switchHysteresis = 5;
    int m_goodRssiThreshold = -60;
    int m_failBlockBaseMs = 10000;                 // 首次失败冷却
    int m_failBlockMaxMs = 120000;                 // 冷却上限
    int m_timerId = INVAILD_TIMER_ID;

    std::unordered_map<std::string, JZWifiFailInfo> m_failBlacklist;

    std::string m_connectedSsid;
    int m_connectedRssi = -127;
    bool m_connecting = false;
    std::string m_pendingSsid;
    int m_pendingRssi = -127;

    std::vector<JZWifiScanResult> m_lastResults;
};

#endif // !JZ_WIFI_AUTO_CONNECT_SERVICE_H_
