#ifndef JZ_OTA_H_
#define JZ_OTA_H_

#include "JZObject.h"
#include "JZOTADriver.h"
#include <string>
#include <cstdint>

class JZNetworkAccessManager;
class JZNetworkReply;

/*
 * JZOTA: OTA 升级客户端(仅客户端)
 *
 * 流程:
 *   1. start(infoUrl)                            -> 获取升级信息(manifest JSON)
 *   2. 解析出 JZOTAUpdateInfo, 发射 sigInfoReady  -> 交由 JZOTADriver::verify 校验
 *   3. 校验通过                                  -> 使用 HTTP Range 分块下载, 逐块写入 driver
 *   4. 全部写入完成                               -> 发射 sigFinished; 失败发射 sigError
 *
 * 写入与校验行为由外部注入的 JZOTADriver(固件驱动, 如 JZOTAESP32Driver)决定。
 * 仅更新文件 / 资源(配置、脚本、模型等)请改用独立的 JZOTAFileDriver。
 */
class JZOTA : public JZObject
{
public:
    enum State {
        Idle,
        Checking,     // 获取升级信息中
        Downloading,  // 分块下载 / 写入中
        Writing,
        Finished,
        NoUpdate,     // 已是最新, 无需升级
        Error,
    };

    JZOTA(JZObject *parent = nullptr);
    ~JZOTA();

    // 设置网络访问管理器(可复用已有实例)。为空时 start() 内部自动创建一个
    void setNetworkAccessManager(JZNetworkAccessManager *manager);
    // 设置写入驱动(必填)
    void setDriver(JZOTADriver *driver);

    // 设置分块大小(字节), 默认 64KB
    void setChunkSize(int64_t chunkSize);

    // 启动 OTA: 从 infoUrl 获取升级信息并按流程执行
    bool start(const std::string &infoUrl);

    void cancel();
    bool isRunning() const;

    State state() const;
    double progress() const;                 // [0, 1]
    const JZOTAUpdateInfo &info() const;     // 最近一次获取的升级信息

jz_signals:
    JZSigVoid sigStarted;
    JZSigVoid sigInfoReady;    // 升级信息已获取, 可通过 info() 读取
    JZSigDouble sigProgress;   // 写入进度 0..1
    JZSigString sigError;      // 错误信息
    JZSigVoid sigNoUpdate;     // 已是最新, 无需升级
    JZSigVoid sigFinished;     // 成功完成

protected:
    void onInfoFinished();
    void startDownload();
    void downloadChunk();
    void onChunkFinished();
    void onFullFinished();      // size 未知时的整体下载
    void finishSuccess();

    bool parseInfo(const std::string &text, JZOTAUpdateInfo &out);

    JZNetworkAccessManager *m_manager = nullptr;
    JZOTADriver *m_driver = nullptr;
    JZNetworkReply *m_reply = nullptr;

    State m_state = Idle;
    std::string m_infoUrl;
    JZOTAUpdateInfo m_info;
    int64_t m_chunkSize = 64 * 1024;
    int64_t m_total = 0;
    int64_t m_offset = 0;
    int64_t m_written = 0;
    bool m_ownManager = false;
    bool m_useRange = false;
};

#endif // !JZ_OTA_H_
