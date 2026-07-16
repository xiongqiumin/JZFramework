#include "JZOTAESP32Driver.h"
#include "JZDebug.h"
#include "esp_system.h"

#ifndef OTA_SIZE_UNKNOWN
#define OTA_SIZE_UNKNOWN ((size_t)-1)
#endif

JZOTAESP32Driver::JZOTAESP32Driver()
{
}

JZOTAESP32Driver::~JZOTAESP32Driver()
{
}

bool JZOTAESP32Driver::verify(const JZOTAUpdateInfo &info)
{
    m_updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (!m_updatePartition)
    {
        jzError() << "JZOTAESP32Driver: no OTA update partition found";
        return false;
    }

    // 容量检查: 已知大小且超出分区则拒绝
    if (info.size > 0 && (int64_t)m_updatePartition->size < info.size)
    {
        jzError() << "JZOTAESP32Driver: firmware too large" << info.size
                  << " > partition" << m_updatePartition->size;
        return false;
    }

    jzDebug() << "JZOTAESP32Driver: update partition=" << m_updatePartition->label
              << " size=" << m_updatePartition->size;
    return true;
}

int64_t JZOTAESP32Driver::totalSize()
{
    return m_total;
}

bool JZOTAESP32Driver::begin(int64_t totalSize)
{
    m_total = totalSize;
    m_written = 0;

    if (!m_updatePartition)
    {
        m_updatePartition = esp_ota_get_next_update_partition(nullptr);
        if (!m_updatePartition)
        {
            jzError() << "JZOTAESP32Driver: no OTA update partition";
            return false;
        }
    }

    size_t image_size = (m_total > 0) ? (size_t)m_total : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(m_updatePartition, image_size, &m_otaHandle);
    if (err != ESP_OK)
    {
        jzError() << "JZOTAESP32Driver: esp_ota_begin failed" << err;
        return false;
    }
    return true;
}

int64_t JZOTAESP32Driver::write(int64_t offset, const JZByteArray &buffer)
{
    if (buffer.isEmpty())
        return 0;

    // ESP-IDF OTA 为顺序写入, offset 由内部维护, 此处忽略。
    esp_err_t err = esp_ota_write(m_otaHandle, buffer.data(), (size_t)buffer.size());
    if (err != ESP_OK)
    {
        jzError() << "JZOTAESP32Driver: esp_ota_write failed" << err;
        return -1;
    }
    m_written += buffer.size();
    return (int64_t)buffer.size();
}

bool JZOTAESP32Driver::finish()
{
    esp_err_t err = esp_ota_end(m_otaHandle);
    if (err != ESP_OK)
    {
        // 固件有问题(截断 / 损坏 / 格式不合法): 校验失败, 不置启动分区。
        // 复位句柄与状态, 旧固件照常启动, 且允许后续重试。
        jzError() << "JZOTAESP32Driver: esp_ota_end failed (bad firmware)" << err;
        m_otaHandle = 0;
        m_updatePartition = nullptr;
        m_total = 0;
        m_written = 0;
        return false;
    }

    // 校验通过, 置为下次启动分区(重启后生效)
    err = esp_ota_set_boot_partition(m_updatePartition);
    if (err != ESP_OK)
    {
        jzError() << "JZOTAESP32Driver: esp_ota_set_boot_partition failed" << err;
        m_otaHandle = 0;
        m_updatePartition = nullptr;
        m_total = 0;
        m_written = 0;
        return false;
    }
    m_otaHandle = 0;
    return true;
}

void JZOTAESP32Driver::reboot()
{
    jzDebug() << "JZOTAESP32Driver: restart to apply new firmware";
    esp_restart();
}

void JZOTAESP32Driver::markValid()
{
    // 仅应在新固件已确认可正常运行(如业务初始化 / 网络就绪)后调用。
    // 调用前若崩溃重启, bootloader 会回退到上一有效版本(需开启回滚配置)。
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
        jzWarning() << "JZOTAESP32Driver: mark_app_valid_cancel_rollback failed" << err;
}

void JZOTAESP32Driver::cancel()
{
    // 释放 OTA 句柄(esp_ota_end 会回收句柄资源, 即使镜像不完整校验失败也无妨)。
    // 关键点: 不调用 esp_ota_set_boot_partition, otadata 仍指向旧固件, 不影响启动。
    if (m_otaHandle != 0)
    {
        esp_err_t err = esp_ota_end(m_otaHandle);
        if (err != ESP_OK)
            jzDebug() << "JZOTAESP32Driver: esp_ota_end in cancel (expected if partial)" << err;
        m_otaHandle = 0;
    }
    m_updatePartition = nullptr;
    m_total = 0;
    m_written = 0;
}
