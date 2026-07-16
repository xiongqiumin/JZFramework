#include "JZOTAFileDriver.h"
#include "JZNetworkAccessManager.h"
#include "JZNetworkReply.h"
#include "JZNetworkRequest.h"
#include "JZFile.h"
#include "JZDir.h"
#include "JZCrypto.h"
#include "JZDebug.h"
#include <cstdio>

static std::string jzOtaDirNameOf(const std::string &path)
{
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos)
        return "";
    return path.substr(0, pos);
}

JZOTAFileDriver::JZOTAFileDriver(JZObject *parent)
    :JZObject(parent)
    ,sigStarted(this)
    ,sigProgress(this)
    ,sigError(this)
    ,sigNoUpdate(this)
    ,sigFinished(this)
    ,m_file(new JZFile())
{
}

JZOTAFileDriver::~JZOTAFileDriver()
{
    cancel();
    delete m_file;
    m_file = nullptr;
}

void JZOTAFileDriver::setNetworkAccessManager(JZNetworkAccessManager *manager)
{
    m_manager = manager;
    m_ownManager = false;
}

void JZOTAFileDriver::setFilePath(const std::string &filePath)
{
    m_path = filePath;
}

std::string JZOTAFileDriver::filePath() const
{
    return m_path;
}

void JZOTAFileDriver::setChunkSize(int64_t chunkSize)
{
    if (chunkSize > 0)
        m_chunkSize = chunkSize;
}

std::string JZOTAFileDriver::tempPath() const
{
    return m_path + ".part";
}

bool JZOTAFileDriver::start(const std::string &url, const std::string &expectMd5)
{
    if (m_running)
        return false;

    m_url = url;
    m_expectMd5 = expectMd5;
    m_offset = 0;
    m_written = 0;

    // 已是最新: 目标文件 md5 匹配则跳过
    if (!m_expectMd5.empty() && !m_path.empty() && JZFile::exists(m_path))
    {
        std::string cur = str_fromUtf8(JZCrypto::fileMd5(m_path).toHex());
        if (cur == m_expectMd5)
        {
            sigNoUpdate.emit();
            return true;
        }
    }

    // 确保父目录存在
    std::string dir = jzOtaDirNameOf(m_path);
    if (!dir.empty() && !JZDir(dir).exists())
    {
        if (!JZDir().mkpath(dir))
        {
            jzError() << "JZOTAFileDriver: mkdir failed" << dir;
            fail("mkdir failed: " + dir);
            return false;
        }
    }

    if (!m_file->open(tempPath(), "wb"))
    {
        jzError() << "JZOTAFileDriver: open temp failed" << tempPath();
        fail("open temp failed");
        return false;
    }

    if (!m_manager)
    {
        m_manager = new JZNetworkAccessManager(this);
        m_ownManager = true;
    }

    m_running = true;
    sigStarted.emit();
    downloadChunk();
    return true;
}

void JZOTAFileDriver::downloadChunk()
{
    int64_t end = m_offset + m_chunkSize - 1;
    JZNetworkRequest req(m_url);
    req.setRawHeader("Range", "bytes=" + std::to_string(m_offset) + "-" + std::to_string(end));

    m_reply = m_manager->get(req);
    connect(&m_reply->sigFinished, this, &JZOTAFileDriver::onChunkFinished);
}

void JZOTAFileDriver::onChunkFinished()
{
    if (m_reply->error() != JZNetworkReply::NoError)
    {
        std::string err = "download failed: " + m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        fail(err);
        return;
    }

    int status = m_reply->httpStatusCode();
    JZByteArray data = m_reply->readAll();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (data.isEmpty())
    {
        // 末尾: 之前已写入数据则视为完成, 否则失败
        if (m_written > 0)
            finishSuccess();
        else
            fail("download empty response");
        return;
    }

    // 服务器不支持 Range(返回 200 完整文件): 首个分块即为全部内容, 写完即结束,
    // 避免对不支持 Range 的服务器反复请求整文件导致死循环。
    bool partial = (status == 206);

    m_file->seek((int)m_offset, SEEK_SET);
    int written = m_file->write(data);
    if (written < 0)
    {
        fail("write temp failed");
        return;
    }

    m_offset += data.size();
    m_written = m_offset;
    sigProgress.emit(progress());

    if (!partial || (int64_t)data.size() < m_chunkSize)
        finishSuccess();
    else
        downloadChunk();
}

void JZOTAFileDriver::finishSuccess()
{
    m_file->close();

    // 完整性校验
    if (!m_expectMd5.empty())
    {
        std::string cur = str_fromUtf8(JZCrypto::fileMd5(tempPath()).toHex());
        if (cur != m_expectMd5)
        {
            jzError() << "JZOTAFileDriver: md5 mismatch, expect=" << m_expectMd5
                      << " actual=" << cur;
            cleanupTemp();
            m_running = false;
            sigError.emit("md5 mismatch");
            return;
        }
    }

    // 原子替换: 先删目标(Windows 下 rename 不能覆盖), 再重命名临时文件
    if (!m_path.empty() && JZFile::exists(m_path))
        JZFile::remove(m_path);
    if (std::rename(tempPath().c_str(), m_path.c_str()) != 0)
    {
        jzError() << "JZOTAFileDriver: rename failed" << tempPath() << "->" << m_path;
        cleanupTemp();
        m_running = false;
        sigError.emit("rename temp failed");
        return;
    }

    m_running = false;
    sigFinished.emit();
}

void JZOTAFileDriver::fail(const std::string &err)
{
    if (m_reply)
    {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    cleanupTemp();
    m_running = false;
    sigError.emit(err);
}

void JZOTAFileDriver::cancel()
{
    if (m_reply)
    {
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    cleanupTemp();
    m_running = false;
}

void JZOTAFileDriver::cleanupTemp()
{
    if (m_file->isOpen())
        m_file->close();
    if (!tempPath().empty() && JZFile::exists(tempPath()))
    {
        if (!JZFile::remove(tempPath()))
            jzWarning() << "JZOTAFileDriver: remove temp failed" << tempPath();
    }
}

bool JZOTAFileDriver::isRunning() const
{
    return m_running;
}

double JZOTAFileDriver::progress() const
{
    // 顺序 Range 下载无法预先获知总大小, 运行中进度未知
    if (m_running)
        return -1.0;
    return m_written > 0 ? 1.0 : 0.0;
}
