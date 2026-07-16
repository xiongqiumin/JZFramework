#ifndef JZ_OTA_FILE_DRIVER_H_
#define JZ_OTA_FILE_DRIVER_H_

#include "JZOTADriver.h"
#include "JZFile.h"
#include <string>

/*
 * JZOTAFileDriver: 基于 JZFile 的文件写入驱动
 * 将下载到的固件数据写入本地文件, 写入前会自动创建不存在的父目录。
 */
class JZOTAFileDriver : public JZOTADriver
{
public:
    JZOTAFileDriver();
    explicit JZOTAFileDriver(const std::string &filePath);

    void setFilePath(const std::string &filePath);
    std::string filePath() const;

    virtual bool verify(const JZOTAUpdateInfo &info) override;
    virtual int64_t totalSize() override;
    virtual bool begin(int64_t totalSize) override;
    virtual int64_t write(int64_t offset, const JZByteArray &buffer) override;
    virtual bool finish() override;
    virtual void cancel() override;
    virtual void reboot() override;

private:
    std::string m_path;
    JZFile m_file;
    int64_t m_total = 0;
    std::string m_expectMd5;   // 期望 md5, 来自 verify 传入的升级信息
};

#endif // !JZ_OTA_FILE_DRIVER_H_
