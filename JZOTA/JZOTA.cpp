#include "JZOTA.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "json/json.h"
#include "JZDebug.h"

JZOTA::JZOTA(JZObject *parent)
    :JZObject(parent)
    ,sigStarted(this)
    ,sigInfoReady(this)
    ,sigProgress(this)
    ,sigError(this)
    ,sigNoUpdate(this)
    ,sigFinished(this)
{
}

JZOTA::~JZOTA()
{
    cancel();
}

void JZOTA::setNetworkAccessManager(JZNetworkAccessManager *manager)
{
    m_manager = manager;
    m_ownManager = false;
}

void JZOTA::setDriver(JZOTADriver *driver)
{
    m_driver = driver;
}

void JZOTA::setChunkSize(int64_t chunkSize)
{
    if (chunkSize > 0)
        m_chunkSize = chunkSize;
}

bool JZOTA::start(const std::string &infoUrl)
{
    if (m_state != Idle)
        return false;

    if (!m_driver)
    {
        jzError() << "JZOTA: driver not set";
        return false;
    }

    if (!m_manager)
    {
        m_manager = new JZNetworkAccessManager(this);
        m_ownManager = true;
    }

    m_infoUrl = infoUrl;
    m_state = Checking;
    sigStarted.emit();

    JZNetworkRequest req(infoUrl);
    m_reply = m_manager->get(req);
    connect(&m_reply->sigFinished, this, &JZOTA::onInfoFinished);
    return true;
}

void JZOTA::cancel()
{
    if (m_reply)
    {
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    if (m_driver && (m_state == Downloading || m_state == Writing))
        m_driver->cancel();

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
    out.channel  = root.isMember("channel")  ? root["channel"].asString()  : "";
    // 设备匹配
    out.productId  = root.isMember("productId")  ? root["productId"].asString()  : "";
    out.hwVersion  = root.isMember("hwVersion")  ? root["hwVersion"].asString()  : "";
    out.minVersion = root.isMember("minVersion") ? root["minVersion"].asString() : "";
    // 下载
    out.url    = root.isMember("url")    ? root["url"].asString()    : "";
    out.mirror = root.isMember("mirror") ? root["mirror"].asString() : "";
    out.size   = root.isMember("size")   ? root["size"].asInt64()     : 0;
    // 完整性 / 安全
    out.md5       = root.isMember("md5")       ? root["md5"].asString()       : "";
    out.sha256    = root.isMember("sha256")    ? root["sha256"].asString()    : "";
    out.signature = root.isMember("signature") ? root["signature"].asString() : "";
    out.signAlgo  = root.isMember("signAlgo")  ? root["signAlgo"].asString()  : "";
    // 升级策略
    out.mandatory   = root.isMember("mandatory")   ? root["mandatory"].asBool()   : false;
    out.reboot      = root.isMember("reboot")      ? root["reboot"].asBool()     : true;
    out.applyMode   = root.isMember("applyMode")   ? root["applyMode"].asString(): "";
    out.releaseNote = root.isMember("releaseNote") ? root["releaseNote"].asString() : "";
    out.publishTime = root.isMember("publishTime") ? root["publishTime"].asString() : "";
    // 源
    out.infoUrl = root.isMember("infoUrl") ? root["infoUrl"].asString() : "";
    return true;
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
    m_info = info;
    sigInfoReady.emit();

    if (m_info.url.empty())
    {
        m_state = Error;
        sigError.emit("info invalid: empty url");
        return;
    }

    // 交由驱动校验(如: 已是最新则跳过)
    if (!m_driver->verify(m_info))
    {
        m_state = NoUpdate;
        sigNoUpdate.emit();
        return;
    }

    startDownload();
}

void JZOTA::startDownload()
{
    m_total = m_info.size;
    m_offset = 0;
    m_written = 0;
    m_state = Downloading;

    if (m_info.size > 0)
    {
        // 已知大小 -> HTTP Range 分块下载
        m_useRange = true;
        if (!m_driver->begin(m_total))
        {
            m_state = Error;
            sigError.emit("driver begin failed");
            return;
        }
        downloadChunk();
    }
    else
    {
        // 未知大小 -> 整体下载
        m_useRange = false;
        JZNetworkRequest req(m_info.url);
        m_reply = m_manager->get(req);
        connect(&m_reply->sigFinished, this, &JZOTA::onFullFinished);
    }
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

    m_reply = m_manager->get(req);
    connect(&m_reply->sigFinished, this, &JZOTA::onChunkFinished);
}

void JZOTA::onChunkFinished()
{
    if (m_reply->error() != JZNetworkReply::NoError)
    {
        std::string err = "download failed: " + m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        m_driver->finish();
        m_state = Error;
        sigError.emit(err);
        return;
    }

    JZByteArray data = m_reply->readAll();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (data.isEmpty())
    {
        m_driver->finish();
        m_state = Error;
        sigError.emit("download empty chunk");
        return;
    }

    int64_t written = m_driver->write(m_offset, data);
    if (written < 0)
    {
        m_driver->finish();
        m_state = Error;
        sigError.emit("driver write failed");
        return;
    }

    m_offset += data.size();
    m_written = m_offset;
    sigProgress.emit(progress());

    downloadChunk();
}

void JZOTA::onFullFinished()
{
    if (m_reply->error() != JZNetworkReply::NoError)
    {
        std::string err = "download failed: " + m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        m_state = Error;
        sigError.emit(err);
        return;
    }

    JZByteArray data = m_reply->readAll();
    m_reply->deleteLater();
    m_reply = nullptr;

    m_total = data.size();
    m_state = Writing;

    if (!m_driver->begin(m_total))
    {
        m_state = Error;
        sigError.emit("driver begin failed");
        return;
    }

    int64_t written = m_driver->write(0, data);
    if (written < 0)
    {
        m_driver->finish();
        m_state = Error;
        sigError.emit("driver write failed");
        return;
    }
    m_written = written;
    sigProgress.emit(progress());

    if (!m_driver->finish())
    {
        m_state = Error;
        sigError.emit("driver finish failed");
        return;
    }

    m_state = Finished;
    sigFinished.emit();

    // 按服务端策略决定是否重启以生效新固件
    if (m_info.reboot)
        m_driver->reboot();
}

void JZOTA::finishSuccess()
{
    if (!m_driver->finish())
    {
        m_state = Error;
        sigError.emit("driver finish failed");
        return;
    }
    m_state = Finished;
    sigFinished.emit();

    // 按服务端策略决定是否重启以生效新固件
    if (m_info.reboot)
        m_driver->reboot();
}
