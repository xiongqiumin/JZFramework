#ifndef JZ_WIFI_AUTO_CONNECT_SERVICE_H_
#define JZ_WIFI_AUTO_CONNECT_SERVICE_H_

#include "JZObject.h"
#include "JZEvent.h"
#include "JZSignal.h"
#include "JZWifiDriver.h"
#include "JZWifiStation.h"
#include "JZWifiScanner.h"
#include "JZPlatform.h"
#include "JZTimer.h"
#include <string>
#include <vector>
#include <unordered_map>

// 自动连接候选: ssid + 密码
struct JZWifiCredential
{
    std::string ssid;
    std::string password;
};

// 自动连接服务配置
struct JZWifiAutoConnectServiceConfig
{
    std::vector<JZWifiCredential> candidates;   // 候选网络序列
    int scanIntervalMs = 15000;                 // 基础扫描间隔(毫秒)
    int maxBackoffMs = 60000;                   // 退避上限(毫秒)
    int switchHysteresis = 5;                   // 切换迟滞(dBm): 仅当新候选比当前强出该值才切换
    int poorRssiThreshold = -75;                // 信号差阈值(dBm): 已连接时低于此值才重新扫描以漫游, 否则跳过周期扫描省电
    int failBlockBaseMs = 10000;                // 首次失败冷却(毫秒)
    int failBlockMaxMs = 120000;               // 冷却上限(毫秒)
    int rssiIntervalMs = 15000;                // RSSI 轮询间隔(毫秒)
};

struct JZWifiConnectInfo
{
    std::string ssid;
    int rssi;
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

    void setConfig(const JZWifiAutoConnectServiceConfig &config);
    JZWifiAutoConnectServiceConfig config() const;

    // 失败黑名单: 手动清空(不传 ssid 则全部清空)
    void clearFailBlacklist(const std::string &ssid = std::string());

    void start();
    void stop();
    bool isActive() const;

    // 当前已连接信息(未连接时 ssid 为空)
    bool isConnected() const;
    JZWifiConnectInfo connectedInfo() const;

    // 最近一次扫描结果
    JZWifiScanResult lastScanResults() const;

jz_signals:
    JZSignal<> sigScanning;                       // 开始扫描
    JZSignal<const std::string&> sigConnecting;   // 正在连接某 ssid
    JZSignal<const std::string&> sigConnected;    // 已连接到某 ssid
    JZSignal<> sigDisconnected;  
    JZSignal<int, const std::string&> sigError;
    JZSignal<int> sigRssiChanged;                 // 已连接网络 RSSI 变化, 参数为 dBm

protected:
    // 失败黑名单条目: 记录某 ssid 连续连接失败次数与冷却到期时间戳
    struct WifiFailInfo
    {
        int failCount = 0;        // 连续失败次数
        int64_t blockUntil = 0;   // 冷却到期时间(currentMSecsSinceEpoch, 毫秒)
    };

    void timerEvent(JZTimerEvent *event) override;

    void onScanFinished(const JZWifiScanResult &result);
    void onScanError(int code, const std::string &msg);

    void onStationConnected();
    void onStationDisconnected();
    void onStationError(int code, const std::string &msg);
    void onStationRssiChanged(int rssi);

    void onConnectTimer();
    
    bool isStationConnecting();
    void evaluateAndConnect();
    void armTimer();                              // 按当前 m_effectiveInterval 重启周期定时器    

    JZWifiScanner *m_scanner;
    JZWifiStation *m_station;

    JZWifiAutoConnectServiceConfig m_config;
    bool m_active = false;
    int m_effectiveInterval = 15000;              // 当前生效间隔(退避时翻倍)
    int m_timerId = INVAILD_TIMER_ID;

    std::map<std::string, WifiFailInfo> m_failBlacklist;

    std::string m_connectedSsid;
    int m_connectedRssi = -127;
    JZWifiCredential m_pendingSsid;
    int m_pendingRssi = -127;

    JZWifiScanResult m_lastResults;
    JZTimer *m_connectTimer;
};

#endif // !JZ_WIFI_AUTO_CONNECT_SERVICE_H_
