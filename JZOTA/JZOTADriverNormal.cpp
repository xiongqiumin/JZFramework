#include "JZOTADriverNormal.h"
#include "JZOTA.h"
#include "JZPlatform.h"
#include "JZDir.h"
#include "JZFile.h"
#include "JZProcess.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZDebug.h"
#include "json/json.h"
#ifdef HAS_JZCompress
#include "JZCompress.h"
#endif

#include <ctime>
#include <vector>

JZ_OBJECT_IMPL(JZOTADriverNormal, JZObject)

static std::string otaJoinPath(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    if (dir.back() == '/' || dir.back() == '\\')
        return dir + name;
    return dir + "/" + name;
}

JZOTADriverNormal::JZOTADriverNormal()
    :m_manager(this)
{
}

JZOTADriverNormal::~JZOTADriverNormal()
{
    if (m_file.is_open())
        m_file.close();
    if (m_reportReply)
    {
        m_reportReply->deleteLater();
        m_reportReply = nullptr;
    }
}

std::string JZOTADriverNormal::otaDir()
{
    return otaJoinPath(JZPlatform::applicationDirPath(), "ota");
}

std::string JZOTADriverNormal::packagePath()
{
    return otaJoinPath(otaDir(), "package.zip");
}

std::string JZOTADriverNormal::updaterPath()
{
#ifdef _WIN32
    return otaJoinPath(JZPlatform::applicationDirPath(), "jzupdater.exe");
#else
    return otaJoinPath(JZPlatform::applicationDirPath(), "jzupdater");
#endif
}

static std::string otaFlagPath()
{
    return otaJoinPath(JZOTADriverNormal::otaDir(), "update_flag.txt");
}

JZResult<void> JZOTADriverNormal::verify(const JZOTAUpdateInfo &info)
{
    // 记录待升级信息, finish() 时用于组装差异信息
    m_toVersion = info.version;
    m_productId = info.productId;
    m_hwVersion = info.hwVersion;
    m_extra = info.extra;

    // 桌面升级依赖独立更新器进程
    std::string path = updaterPath();
    if (!JZFile::exists(path))
        return JZResult<void>::Error("updater not found: " + path);
    return JZResult<void>::Ok();
}

int64_t JZOTADriverNormal::totalSize()
{
    return m_totalSize;
}

bool JZOTADriverNormal::begin(int64_t totalSize)
{
    m_totalSize = totalSize;
    m_packagePath = packagePath();

    JZDir dir;
    if (!JZFile::exists(otaDir()) && !dir.mkpath(otaDir()))
    {
        jzError() << "JZOTADriverNormal: create ota dir failed" << otaDir();
        return false;
    }

    // 清掉上次残留的半成品包
    JZFile::remove(m_packagePath);

    m_file.open(m_packagePath, std::ios::binary | std::ios::trunc);
    if (!m_file.is_open())
    {
        jzError() << "JZOTADriverNormal: open package failed" << m_packagePath;
        return false;
    }
    return true;
}

int64_t JZOTADriverNormal::write(int64_t offset, const JZByteArray &buffer)
{
    if (!m_file.is_open())
        return -1;

    m_file.seekp(offset);
    if (!m_file.good())
        return -1;

    if (buffer.size() > 0)
        m_file.write(buffer.data(), buffer.size());
    if (!m_file.good())
        return -1;

    return buffer.size();
}

bool JZOTADriverNormal::finish()
{
    if (m_file.is_open())
        m_file.close();

    // 校验落盘大小与期望一致(防下载不完整)
    if (m_totalSize > 0)
    {
        JZFile f;
        if (!f.open(m_packagePath, "rb"))
        {
            jzError() << "JZOTADriverNormal: open downloaded package failed" << m_packagePath;
            return false;
        }
        int64_t size = f.size();
        f.close();
        if (size != m_totalSize)
        {
            jzError() << "JZOTADriverNormal: package size mismatch, expect"
                      << m_totalSize << "got" << size;
            return false;
        }
    }

    uploadDiffInfo();
    return true;
}

void JZOTADriverNormal::cancel()
{
    if (m_file.is_open())
        m_file.close();

    // 中止升级: 清理半成品包, 不影响当前应用运行
    if (!m_packagePath.empty())
        JZFile::remove(m_packagePath);

    if (m_reportReply)
    {
        m_reportReply->deleteLater();
        m_reportReply = nullptr;
    }

    m_totalSize = 0;
}

void JZOTADriverNormal::uploadDiffInfo()
{
    // 上报地址取 manifest 扩展字段 extra["reportUrl"], 未配置则跳过
    std::string url = m_extra.isMember("reportUrl") ? m_extra["reportUrl"].asString() : "";
    if (url.empty() || m_reportReply)
        return;

    Json::Value root;
    root["type"] = "ota_diff";
    root["productId"] = m_productId;
    root["hwVersion"] = m_hwVersion;
    root["toVersion"] = m_toVersion;

    // 当前版本: 内置 meta (资源或 <otaDir>/meta.json)
    std::string fromVersion;
    auto metaRes = JZOTA::readCurrentMeta();
    if (metaRes.isOk())
        fromVersion = metaRes.value().version;
    root["fromVersion"] = fromVersion;

    int64_t pkgSize = 0;
    JZFile f;
    if (f.open(m_packagePath, "rb"))
    {
        pkgSize = f.size();
        f.close();
    }
    root["packageSize"] = (Json::Int64)pkgSize;
    root["time"] = (Json::Int64)std::time(nullptr);

#ifdef HAS_JZCompress
    // 包内文件清单(差异内容)
    Json::Value files(Json::arrayValue);
    JZUnZip zip;
    if (zip.open(m_packagePath).isOk())
    {
        auto entries = zip.list();
        if (entries.isOk())
        {
            for (const auto &e : entries.value())
            {
                Json::Value item;
                item["name"] = e.name;
                item["size"] = (Json::Int64)e.size;
                item["compressedSize"] = (Json::Int64)e.compressedSize;
                files.append(item);
            }
        }
    }
    root["files"] = files;
#endif

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    std::string body = Json::writeString(wb, root);

    JZNetworkRequest req(url);
    req.setHeader(JZNetworkRequest::ContentTypeHeader, "application/json");
    m_reportReply = m_manager.post(req, JZByteArray(body.data(), (int)body.size()));
    connect(&m_reportReply->sigFinished, this, &JZOTADriverNormal::onReportFinished);
}

void JZOTADriverNormal::onReportFinished()
{
    if (!m_reportReply)
        return;

    if (m_reportReply->error() != JZNetworkReply::NoError)
        jzWarning() << "JZOTADriverNormal: upload diff info failed:"
                    << m_reportReply->errorString();
    else
        jzDebug() << "JZOTADriverNormal: diff info uploaded";

    m_reportReply->deleteLater();
    m_reportReply = nullptr;
}

void JZOTADriverNormal::reboot()
{
    // 写升级标记: 下次启动 isOTAReboot() 为 true, 自检成功后 markValid() 清除
    {
        std::ofstream flag(otaFlagPath(), std::ios::binary | std::ios::trunc);
        if (flag.is_open())
            flag << "update\n";
    }

    // 写升级任务文件, 由 jzupdater 读取执行
    Json::Value job;
    job["package"] = m_packagePath;
    job["appDir"] = JZPlatform::applicationDirPath();
    job["appFile"] = JZPlatform::applicationFilePath();
    job["restart"] = true;

    std::string jobPath = otaJoinPath(otaDir(), "job.json");
    {
        std::ofstream out(jobPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            jzError() << "JZOTADriverNormal: write job failed" << jobPath;
            return;
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        out << Json::writeString(wb, job);
    }

    if (!JZProcess::startDetached(updaterPath(), { jobPath }))
    {
        jzError() << "JZOTADriverNormal: start updater failed" << updaterPath();
        return;
    }

    jzDebug() << "JZOTADriverNormal: updater started" << updaterPath() << ", exiting";
    exit(0);
}

void JZOTADriverNormal::markValid()
{
    // 新版本确认运行正常: 清除升级标记, 观察期结束
    std::string path = otaFlagPath();
    if (std::remove(path.c_str()) == 0)
        jzDebug() << "JZOTADriverNormal: OTA reboot flag cleared" << path;
}

bool JZOTADriverNormal::isOTAReboot()
{
    std::ifstream f(otaFlagPath());
    return f.is_open();
}

void JZOTADriverNormal::rollback()
{
    // 桌面平台无上一版本可回退, 仅清理标记
    std::remove(otaFlagPath().c_str());
    jzWarning() << "JZOTADriverNormal: rollback not supported, flag cleared";
}
