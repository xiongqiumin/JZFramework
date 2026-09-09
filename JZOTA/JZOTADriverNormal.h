#ifndef JZ_OTA_DRIVER_NORMAL_H_
#define JZ_OTA_DRIVER_NORMAL_H_

#include "JZOTADriver.h"
#include "JZObject.h"
#include "JZNetworkAccessManager.h"
#include "json/json.h"

#include <fstream>
#include <string>

class JZNetworkReply;

/*
 * JZOTADriverNormal: OS_NORMAL(桌面 / 常规操作系统) OTA 驱动。
 *
 * 与嵌入式驱动(直接写 Flash)不同, 桌面升级流程为:
 *   1. JZOTA 分块下载升级包(JZCompress zip), 驱动把数据写入 <appDir>/ota/package.zip
 *   2. finish() 关闭文件并(可选)向服务端上传差异信息(版本比对结果 + 包内文件清单),
 *      上报地址取 manifest 的 extra["reportUrl"]
 *   3. reboot() 在 <appDir>/ota/job.json 写入升级任务, 启动 <appDir>/jzupdater
 *      (源码 binary/updater) 后退出本进程; 由 jzupdater 等待主程序退出、
 *      解压升级包覆盖安装目录、安装内置版本信息(meta.json)并重启应用
 *   4. 新版本启动后 isOTAReboot() 为 true, 应用自检成功后调用 markValid() 清除标记
 *
 * 内置版本信息: 桌面平台未编入 ":/JZOTA/meta" 资源时, JZOTA::readCurrentMeta
 * 回退读取 <appDir>/ota/meta.json (由 jzupdater 从升级包安装)。
 */
class JZOTADriverNormal : public JZOTADriver, public JZObject
{
    JZ_OBJECT_DECLARE(JZOTADriverNormal)
public:
    JZOTADriverNormal();
    ~JZOTADriverNormal() override;

    // OTA 工作目录 <appDir>/ota (内置 meta / 升级包 / 任务文件 / 标记文件均存放于此)
    static std::string otaDir();
    // 升级包路径 <otaDir>/package.zip
    static std::string packagePath();
    // 更新器可执行文件路径 <appDir>/jzupdater(.exe)
    static std::string updaterPath();

    JZResult<void> verify(const JZOTAUpdateInfo &info) override;
    int64_t totalSize() override;
    bool begin(int64_t totalSize) override;
    int64_t write(int64_t offset, const JZByteArray &buffer) override;
    bool finish() override;
    void cancel() override;
    void reboot() override;

    void markValid() override;
    bool isOTAReboot() override;
    void rollback() override;

protected:
    // 上传差异信息(fire-and-forget, 不阻塞升级流程)
    void uploadDiffInfo();
    void onReportFinished();

    std::ofstream m_file;
    std::string m_packagePath;
    int64_t m_totalSize = 0;

    // 由 verify() 记录的待升级信息(用于 finish() 上传差异信息)
    std::string m_toVersion;
    std::string m_productId;
    std::string m_hwVersion;
    Json::Value m_extra;

    JZNetworkAccessManager m_manager;
    JZNetworkReply *m_reportReply = nullptr;
};

#endif // !JZ_OTA_DRIVER_NORMAL_H_
