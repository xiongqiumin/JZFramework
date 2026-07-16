#include "JZOTAFileDriver.h"
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

JZOTAFileDriver::JZOTAFileDriver()
{
}

JZOTAFileDriver::JZOTAFileDriver(const std::string &filePath)
    : m_path(filePath)
{
}

void JZOTAFileDriver::setFilePath(const std::string &filePath)
{
    m_path = filePath;
}

std::string JZOTAFileDriver::filePath() const
{
    return m_path;
}

int64_t JZOTAFileDriver::totalSize()
{
    return m_total;
}

bool JZOTAFileDriver::verify(const JZOTAUpdateInfo &info)
{
    m_expectMd5 = info.md5;

    // 已存在且 md5 一致 -> 已是最新, 无需升级
    if (!m_expectMd5.empty() && JZFile::exists(m_path))
    {
        std::string cur = str_fromUtf8(JZCrypto::fileMd5(m_path).toHex());
        if (cur == m_expectMd5)
            return false;
    }
    return true;
}

bool JZOTAFileDriver::begin(int64_t totalSize)
{
    m_total = totalSize;

    std::string dir = jzOtaDirNameOf(m_path);
    if (!dir.empty() && !JZDir(dir).exists())
    {
        if (!JZDir().mkpath(dir))
        {
            jzError() << "JZOTAFileDriver: mkdir failed" << dir;
            return false;
        }
    }

    if (!m_file.open(m_path, "wb"))
    {
        jzError() << "JZOTAFileDriver: open failed" << m_path;
        return false;
    }
    return true;
}

int64_t JZOTAFileDriver::write(int64_t offset, const JZByteArray &buffer)
{
    m_file.seek((int)offset, SEEK_SET);
    return m_file.write(buffer);
}

bool JZOTAFileDriver::finish()
{
    m_file.close();

    // 写入完成后做完整性校验
    if (!m_expectMd5.empty() && JZFile::exists(m_path))
    {
        std::string cur = str_fromUtf8(JZCrypto::fileMd5(m_path).toHex());
        if (cur != m_expectMd5)
        {
            jzError() << "JZOTAFileDriver: md5 mismatch after write, expect=" << m_expectMd5
                      << " actual=" << cur;
            return false;
        }
    }
    return true;
}

void JZOTAFileDriver::reboot()
{
    // 文件/主机场景无需重启以生效(新固件即落盘文件), 此处为空操作。
    // 真实设备驱动应在此置 boot 标志并调用平台 reboot()。
    jzDebug() << "JZOTAFileDriver: reboot (no-op for file target)";
}

void JZOTAFileDriver::cancel()
{
    m_file.close();
    m_total = 0;
    m_expectMd5.clear();

    // 删除半成品文件, 避免下次 verify 误判 / 残留损坏文件
    if (!m_path.empty() && JZFile::exists(m_path))
    {
        if (!JZFile::remove(m_path))
            jzWarning() << "JZOTAFileDriver: remove partial file failed" << m_path;
    }
}
