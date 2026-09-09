#ifndef JZ_OTA_H_
#define JZ_OTA_H_

#include "JZObject.h"
#include "JZOTADriver.h"
#include <string>
#include <cstdint>

#include "json/json.h"
#include "JZNetworkAccessManager.h"
class JZNetworkReply;

// 升级信息(由服务端 manifest 提供, 服务端一定支持 HTTP Range)
struct JZOTAUpdateInfo {
    // ---------- 标识 / 版本 ----------
    std::string version;     // 目标固件版本号
    std::string buildId;     // 构建号 / commit hash

    // ---------- 设备匹配(防刷错固件, 由 driver 决定如何校验) ----------
    std::string productId;   // 产品 / 型号 id, 不匹配应拒绝升级
    std::string hwVersion;   // 目标硬件版本, 不兼容应拒绝升级
    std::string minVersion;  // 允许升级的最低当前版本(防跨版本不兼容)

    // ---------- 下载 ----------
    std::string url;         // 主下载地址    
    int64_t size = 0;        // 固件总大小(字节), <=0 表示未知(此时整体下载)

    // 签名
    std::string signature;

    // ---------- 升级策略 / 扩展(供上层调度 / UI 使用) ----------    
    // 上层可通过 extra["reboot"].asBool() 等直接读取
    Json::Value extra;

    // ---------- 校验结果(由 JZOTA::queryOTAInfo 填充) ----------
    bool needUpdate = false; // 经 driver 校验后是否需要升级; true=需要, false=无需(如已是最新)
};

// 当前固件自描述元数据: 作为资源 ":/JZOTA/meta" 编入镜像, 由 JZOTA::readCurrentMeta 读取用于校验。
// 元数据随镜像一起分发(与 bin 一体), 升级后新固件自带自己的 meta。
struct JZOTAMeta {
    std::string version;     // 当前固件版本号
    std::string productId;   // 产品 / 型号 id(用于防刷错机型)
    std::string hwVersion;   // 硬件版本(用于兼容性校验)
    int64_t upgradeTime = 0; // 上次升级时间(epoch 秒), 0=未知
    std::string infoUrl;     // 上次升级来源 manifest 地址
    std::string data;        // 扩展数据(透传的自由字符串, 如备注 / 自定义字段)
};


/*
 * JZOTA: OTA 升级客户端(仅客户端)
 *
 * 流程:
 *   1. queryOTAInfo(infoUrl)                     -> 获取并校验升级信息, 结果存入 info().needUpdate, 发射 sigInfoReady
 *   2. 调用方依据 info().needUpdate 决定是否继续
 *   3. startOTA()                                -> 使用 HTTP Range 分块下载, 逐块写入 driver(不再校验)
 *   4. 全部写入完成                               -> 发射 sigFinished; 失败发射 sigError
 *
 * 写入与校验行为由 JZOTADriver(固件驱动, 如 JZOTADriverEsp32)决定,
 * 通过 JZOTADriver::instance() 获取; 驱动实例在 JZDriver::init 中按平台安装。
 */
class JZOTA : public JZObject
{
public:
    // 读取当前固件自描述元数据(资源 ":/JZOTA/meta"; 桌面平台回退 <appDir>/ota/meta.json)。
    // Ok(meta)=读取并解析成功; Error=资源缺失或解析失败。
    static JZResult<JZOTAMeta> readCurrentMeta();

    // 确认当前运行固件有效(防回滚)。应在业务初始化自检成功后才调用。
    void markValid();

    // 当前启动是否为 OTA 升级后的首次启动(固件处于 PENDING_VERIFY 状态)。
    bool isOTAReboot();

    // 主动回滚: 标记当前固件无效, 回退到上一有效版本并重启。
    // 调用后设备重启, 不会返回。
    void rollback();

    enum State {
        Idle,
        Checking,     // 获取升级信息中
        Downloading,  // 分块下载 / 写入中
        Writing,
        Finished,
        Error,
    };

    JZOTA(JZObject *parent = nullptr);
    ~JZOTA();

    // 设置分块大小(字节), 默认 64KB
    void setChunkSize(int64_t chunkSize);

    // 下载失败重试次数, 默认 3, 设为 0 则禁用重试
    void setMaxRetries(int maxRetries);
    int maxRetries() const;

    // 获取升级信息(manifest JSON), 成功后发射 sigInfoReady; 不会自动开始升级
    void queryOTAInfo(const std::string &infoUrl);    
    const JZOTAUpdateInfo &info() const;     // 最近一次获取的升级信息

    // 在 queryOTAInfo 之后调用: 开始分块下载 / 写入; 返回 false 表示状态错误或信息无效
    bool startOTA();

    // 停止当前 OTA(获取信息或下载 / 写入)
    void stopOTA();

    void cancel();
    bool isRunning() const;

    State state() const;
    double progress() const;                 // [0, 1]

jz_signals:
    JZSigVoid sigInfoReady;    // 升级信息已获取, 可通过 info() 读取
    JZSigDouble sigProgress;   // 写入进度 0..1
    JZSigString sigError;      // 错误信息
    JZSigVoid sigFinished;     // 成功完成
    JZSigInt sigRetry;         // 下载重试, 参数为当前重试次数(1..maxRetries)

protected:
    void onInfoFinished();
    void startDownload();
    void downloadChunk();
    void onChunkFinished();
    void finishSuccess();
    bool retryDownload();

    bool parseInfo(const std::string &text, JZOTAUpdateInfo &out);
    JZResult<bool> verify(const JZOTAUpdateInfo &info);

    JZNetworkAccessManager m_manager;
    JZNetworkReply *m_reply = nullptr;

    State m_state = Idle;
    std::string m_infoUrl;
    JZOTAUpdateInfo m_info;
    int64_t m_chunkSize = 64 * 1024;
    int64_t m_total = 0;
    int64_t m_offset = 0;
    int64_t m_written = 0;
    int m_retryCount = 0;
    int m_maxRetries = 3;
};

#endif // !JZ_OTA_H_
