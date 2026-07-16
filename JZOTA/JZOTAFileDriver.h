#ifndef JZ_OTA_FILE_DRIVER_H_
#define JZ_OTA_FILE_DRIVER_H_

#include "JZObject.h"
#include "JZByteArray.h"
#include <string>
#include <cstdint>

class JZNetworkAccessManager;
class JZNetworkReply;

/*
 * JZOTAFileDriver: 文件 / 资源更新器(独立于固件 OTA)
 *
 * 与固件 OTA(JZOTA + JZOTADriver, 含 reboot / 回滚)不同, 本类专门用于在
 * 不重启设备的前提下, 把远端文件 / 资源(配置、脚本、模型、素材等)下载并
 * 写入本地路径。其特点是:
 *   - 不继承 JZOTADriver, 没有 reboot / markValid 等固件语义
 *   - 下载到临时文件(.part), 成功后才原子替换目标文件; 失败 / 取消时
 *     仅删除临时文件, 原文件保持不动
 *   - 支持可选的 md5(hex) 完整性校验; 若目标文件 md5 已匹配则直接跳过
 *   - 使用 HTTP Range 顺序分块下载(自动探测结尾, 无需服务端 manifest)
 */
class JZOTAFileDriver : public JZObject
{
public:
    JZOTAFileDriver(JZObject *parent = nullptr);
    ~JZOTAFileDriver();

    // 复用已有的网络管理器; 为空时 start() 内部自动创建一个
    void setNetworkAccessManager(JZNetworkAccessManager *manager);
    // 目标文件(资源)路径
    void setFilePath(const std::string &filePath);
    std::string filePath() const;

    // 分块大小(字节), 默认 64KB
    void setChunkSize(int64_t chunkSize);

    // 启动: 从 url 下载并写入 filePath。expectMd5 为空表示不做完整性校验。
    // 返回 false 表示无法启动(如已在运行 / 打开文件失败)。
    bool start(const std::string &url, const std::string &expectMd5 = "");

    // 取消: 中止下载并清理临时文件, 原文件保持不变。
    void cancel();

    bool isRunning() const;
    // 进度 [0,1]; 总大小未知时返回 -1(不确定进度)
    double progress() const;

jz_signals:
    JZSigVoid sigStarted;
    JZSigDouble sigProgress;   // 写入进度 0..1(未知时为 -1)
    JZSigString sigError;      // 错误信息
    JZSigVoid sigNoUpdate;     // 目标文件 md5 已匹配, 无需更新
    JZSigVoid sigFinished;     // 成功完成(已替换目标文件)

protected:
    std::string tempPath() const;
    void downloadChunk();
    void onChunkFinished();
    void finishSuccess();
    void fail(const std::string &err);
    void cleanupTemp();

    JZNetworkAccessManager *m_manager = nullptr;
    JZNetworkReply *m_reply = nullptr;
    class JZFile *m_file = nullptr;

    std::string m_path;
    std::string m_url;
    std::string m_expectMd5;
    int64_t m_chunkSize = 64 * 1024;
    int64_t m_offset = 0;
    int64_t m_written = 0;
    bool m_ownManager = false;
    bool m_running = false;

    JZ_DISABLE_COPY(JZOTAFileDriver)
};

#endif // !JZ_OTA_FILE_DRIVER_H_
