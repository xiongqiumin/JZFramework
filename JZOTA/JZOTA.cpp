#include "JZOTA.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZFile.h"
#include "json/json.h"
#include "JZDebug.h"
#ifdef JZ_OS_NORMAL
#include "JZPlatform.h"
#endif

JZOTA::JZOTA(JZObject *parent)
    :JZObject(parent)
    ,sigInfoReady(this)
    ,sigProgress(this)
    ,sigError(this)
    ,sigFinished(this)
    ,sigRetry(this)
    ,m_manager(this)
{
}

JZOTA::~JZOTA()
{
    cancel();
}

void JZOTA::setChunkSize(int64_t chunkSize)
{
    if (chunkSize > 0)
        m_chunkSize = chunkSize;
}

void JZOTA::setMaxRetries(int maxRetries)
{
    m_maxRetries = maxRetries > 0 ? maxRetries : 0;
}

int JZOTA::maxRetries() const
{
    return m_maxRetries;
}

void JZOTA::queryOTAInfo(const std::string &infoUrl)
{
    if (m_state != Idle)
        return;

    m_infoUrl = infoUrl;
    m_state = Checking;

    JZNetworkRequest req(infoUrl);
    m_reply = m_manager.get(req);
    connect(&m_reply->sigFinished, this, &JZOTA::onInfoFinished);
}

bool JZOTA::startOTA()
{
    if (m_state != Idle)
        return false;
    if (m_info.url.empty())
        return false;

    m_retryCount = 0;
    startDownload();
    return true;
}

void JZOTA::stopOTA()
{
    cancel();
}

void JZOTA::cancel()
{
    if (m_reply)
    {
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    if (m_state == Downloading || m_state == Writing)
        JZOTADriver::instance()->cancel();

    m_retryCount = 0;
    m_state = Idle;
}

bool JZOTA::isRunning() const
{
    return m_state == Checking || m_state == Downloading || m_state == Writing;
}

JZOTA::State JZOTA::state() const
{
    return m_state;
}

double JZOTA::progress() const
{
    if (m_total <= 0)
        return 0.0;
    if (m_state == Finished)
        return 1.0;
    if (m_state == Checking)
        return 0.0;
    return (double)m_written / (double)m_total;
}

JZResult<JZOTAMeta> JZOTA::readCurrentMeta()
{
    static const char *kResource = ":/JZOTA/meta";

    std::string text;
    JZFile file;
    if (file.open(kResource, "rb"))
    {
        JZByteArray data = file.readAll();
        file.close();
        text.assign((const char *)data.data(), (size_t)data.size());
    }
#ifdef JZ_OS_NORMAL
    else
    {
        // 桌面平台回退: 资源未编入时读取 <appDir>/ota/meta.json
        // (内置版本信息, 由 jzupdater 安装升级包时写入)
        JZFile metaFile;
        if (metaFile.open(JZPlatform::applicationDirPath() + "/ota/meta.json", "rb"))
        {
            JZByteArray data = metaFile.readAll();
            metaFile.close();
            text.assign((const char *)data.data(), (size_t)data.size());
        }
    }
#endif

    if (text.empty())
        return JZResult<JZOTAMeta>::Error(std::string("resource not found: ") + kResource);

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (text.empty() || !reader->parse(text.data(), text.data() + text.size(), &root, &errs))
        return JZResult<JZOTAMeta>::Error(std::string("JZOTAMeta parse failed: ") + errs);
    if (!root.isObject())
        return JZResult<JZOTAMeta>::Error("JZOTAMeta invalid format");

    JZOTAMeta meta;
    meta.version    = root.isMember("version")    ? root["version"].asString()    : "";
    meta.productId  = root.isMember("productId")  ? root["productId"].asString()  : "";
    meta.hwVersion  = root.isMember("hwVersion")  ? root["hwVersion"].asString()  : "";
    meta.upgradeTime= root.isMember("upgradeTime")? root["upgradeTime"].asInt64() : 0;
    meta.infoUrl    = root.isMember("infoUrl")    ? root["infoUrl"].asString()    : "";
    meta.data       = root.isMember("data")       ? root["data"].asString()       : "";
    return JZResult<JZOTAMeta>::Ok(meta);
}

void JZOTA::markValid()
{
    JZOTADriver::instance()->markValid();
}

bool JZOTA::isOTAReboot()
{
    return JZOTADriver::instance()->isOTAReboot();
}

void JZOTA::rollback()
{
    JZOTADriver::instance()->rollback();
}

const JZOTAUpdateInfo &JZOTA::info() const
{
    return m_info;
}

bool JZOTA::parseInfo(const std::string &text, JZOTAUpdateInfo &out)
{
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    if (text.empty() || !reader->parse(text.data(), text.data() + text.size(), &root, &errs))
        return false;
    if (!root.isObject())
        return false;

    // 标识 / 版本
    out.version  = root.isMember("version")  ? root["version"].asString()  : "";
    out.buildId  = root.isMember("buildId")  ? root["buildId"].asString()  : "";
    // 设备匹配
    out.productId  = root.isMember("productId")  ? root["productId"].asString()  : "";
    out.hwVersion  = root.isMember("hwVersion")  ? root["hwVersion"].asString()  : "";
    out.minVersion = root.isMember("minVersion") ? root["minVersion"].asString() : "";
    // 下载
    out.url    = root.isMember("url")    ? root["url"].asString()    : "";    
    out.size   = root.isMember("size")   ? root["size"].asInt64()     : 0;
    out.signature = root.isMember("signature") ? root["signature"].asString() : "";
    // 升级策略 / 扩展: 整个 JSON 透传给上层(含 md5/sha256/signAlgo 等)
    out.extra = root;
    return true;
}

JZResult<bool> JZOTA::verify(const JZOTAUpdateInfo &info)
{
    // 驱动校验: 分区存在且容量足够
    auto r = JZOTADriver::instance()->verify(info);
    if (r.isError())
        return JZResult<bool>::Error(r.errorMessage());

    // 读取当前固件自带元数据, 与待升级信息比对
    auto metaRes = JZOTA::readCurrentMeta();
    if (metaRes.isOk())
    {
        const JZOTAMeta &cur = metaRes.value();

        // 版本相同: 已是最新, 无需升级
        if (!cur.version.empty() && cur.version == info.version)
        {
            jzDebug() << "JZOTA: already at version" << info.version << ", skip";
            return JZResult<bool>::Ok(false);
        }

        // 机型 / 硬件不匹配: 拒绝, 防止刷错固件
        if (!cur.productId.empty() && !info.productId.empty() && cur.productId != info.productId)
        {
            jzError() << "JZOTA: product mismatch, current=" << cur.productId
                      << " target=" << info.productId;
            return JZResult<bool>::Error("product mismatch");
        }
        if (!cur.hwVersion.empty() && !info.hwVersion.empty() && cur.hwVersion != info.hwVersion)
        {
            jzError() << "JZOTA: hw version mismatch, current=" << cur.hwVersion
                      << " target=" << info.hwVersion;
            return JZResult<bool>::Error("hw version mismatch");
        }
    }
    else
    {
        jzWarning() << "JZOTA: readCurrentMeta failed:" << metaRes.errorMessage();
    }

    return JZResult<bool>::Ok(true);  // 需要升级
}

void JZOTA::onInfoFinished()
{
    if (m_reply->error() != JZNetworkReply::NoError)
    {
        std::string err = "fetch info failed: " + m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        m_state = Error;
        sigError.emit(err);
        return;
    }

    std::string text = str_fromUtf8(m_reply->readAll());
    m_reply->deleteLater();
    m_reply = nullptr;

    JZOTAUpdateInfo info;
    if (!parseInfo(text, info))
    {
        m_state = Error;
        sigError.emit("parse info failed");
        return;
    }    

    if (info.url.empty())
    {
        m_state = Error;
        sigError.emit("info invalid: empty url");
        return;
    }    

    // 获得信息后自行校验, 结果存入 m_info.needUpdate(不自动开始升级)
    auto r = this->verify(info);
    if (r.isError())
    {
        m_state = Error;
        sigError.emit(r.errorMessage());
        return;
    }
    m_info = info;
    m_info.needUpdate = r.value();

    m_state = Idle;
    sigInfoReady.emit();
}

bool JZOTA::retryDownload()
{
    if (m_retryCount >= m_maxRetries)
        return false;

    m_retryCount++;

    // 清理当前失败的请求
    if (m_reply)
    {
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    jzDebug() << "JZOTA retry chunk #" << m_retryCount << "/" << m_maxRetries;
    sigRetry.emit(m_retryCount);

    // 仅重试当前 chunk: m_offset 未变, driver 句柄仍有效, 无需 cancel
    downloadChunk();
    return true;
}

void JZOTA::startDownload()
{
    m_total = m_info.size;
    m_offset = 0;
    m_written = 0;
    m_state = Downloading;

    if (!JZOTADriver::instance()->begin(m_total))
    {
        m_state = Error;
        sigError.emit("driver begin failed");
        return;
    }
    downloadChunk();
}

void JZOTA::downloadChunk()
{
    if (m_offset >= m_total)
    {
        finishSuccess();
        return;
    }

    int64_t end = m_offset + m_chunkSize - 1;
    if (end >= m_total)
        end = m_total - 1;

    JZNetworkRequest req(m_info.url);
    req.setRawHeader("Range", "bytes=" + std::to_string(m_offset) + "-" + std::to_string(end));

    m_reply = m_manager.get(req);
    connect(&m_reply->sigFinished, this, &JZOTA::onChunkFinished);
}

void JZOTA::onChunkFinished()
{
    if (m_reply->error() != JZNetworkReply::NoError)
    {
        std::string err = "download failed: " + m_reply->errorString();
        if (retryDownload())
            return;
        m_state = Error;
        sigError.emit(err);
        return;
    }

    JZByteArray data = m_reply->readAll();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (data.isEmpty())
    {
        if (retryDownload())
            return;
        m_state = Error;
        sigError.emit("download empty chunk");
        return;
    }

    int64_t written = JZOTADriver::instance()->write(m_offset, data);
    if (written < 0)
    {
        if (retryDownload())
            return;
        m_state = Error;
        sigError.emit("driver write failed");
        return;
    }

    m_offset += data.size();
    m_written = m_offset;
    sigProgress.emit(progress());

    m_retryCount = 0;  // 当前 chunk 成功, 重置以便下一个 chunk 有独立 3 次重试
    downloadChunk();
}

void JZOTA::finishSuccess()
{
    if (!JZOTADriver::instance()->finish())
    {
        m_state = Error;
        sigError.emit("driver finish failed");
        return;
    }
    m_state = Finished;
    sigFinished.emit();

    // 按服务端策略决定是否重启以生效新固件
    bool reboot = m_info.extra.isMember("reboot") ? m_info.extra["reboot"].asBool() : true;
    if (reboot)
        JZOTADriver::instance()->reboot();
}
