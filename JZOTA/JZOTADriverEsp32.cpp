#include "JZOTADriverEsp32.h"
#include "JZDebug.h"
#include "esp_system.h"

#ifndef OTA_SIZE_UNKNOWN
#define OTA_SIZE_UNKNOWN ((size_t)-1)
#endif

JZOTADriverEsp32::JZOTADriverEsp32()
{
}

JZOTADriverEsp32::~JZOTADriverEsp32()
{
}

JZResult<bool> JZOTADriverEsp32::verify(const JZOTAUpdateInfo &info)
{
    m_updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (!m_updatePartition)
    {
        jzError() << "JZOTADriverEsp32: no OTA update partition found";
        return JZResult<bool>::Error("no OTA update partition found");
    }

    // 容量检查: 已知大小且超出分区则拒绝
    if (info.size > 0 && (int64_t)m_updatePartition->size < info.size)
    {
        jzError() << "JZOTADriverEsp32: firmware too large" << info.size
                  << " > partition" << m_updatePartition->size;
        return JZResult<bool>::Error("firmware too large for partition");
    }

    jzDebug() << "JZOTADriverEsp32: update partition=" << m_updatePartition->label
              << " size=" << m_updatePartition->size;

    // 读取当前固件自带元数据(资源 :/JZOTA/meta), 与待升级信息比对
    auto metaRes = JZOTA::readCurrentMeta();
    if (metaRes.isOk())
    {
        const JZOTAMeta &cur = metaRes.value();

        // 版本相同: 已是最新, 无需升级
        if (!cur.version.empty() && cur.version == info.version)
        {
            jzDebug() << "JZOTADriverEsp32: already at version" << info.version << ", skip";
            return JZResult<bool>::Ok(false);
        }

        // 机型 / 硬件不匹配: 拒绝, 防止刷错固件
        if (!cur.productId.empty() && !info.productId.empty() && cur.productId != info.productId)
        {
            jzError() << "JZOTADriverEsp32: product mismatch, current=" << cur.productId
                      << " target=" << info.productId;
            return JZResult<bool>::Error("product mismatch");
        }
        if (!cur.hwVersion.empty() && !info.hwVersion.empty() && cur.hwVersion != info.hwVersion)
        {
            jzError() << "JZOTADriverEsp32: hw version mismatch, current=" << cur.hwVersion
                      << " target=" << info.hwVersion;
            return JZResult<bool>::Error("hw version mismatch");
        }
    }
    else
    {
        jzWarning() << "JZOTADriverEsp32: readCurrentMeta failed:" << metaRes.errorMessage();
    }

    return JZResult<bool>::Ok(true);
}

int64_t JZOTADriverEsp32::totalSize()
{
    return m_total;
}

bool JZOTADriverEsp32::begin(int64_t totalSize)
{
    m_total = totalSize;
    m_written = 0;

    if (!m_updatePartition)
    {
        m_updatePartition = esp_ota_get_next_update_partition(nullptr);
        if (!m_updatePartition)
        {
            jzError() << "JZOTADriverEsp32: no OTA update partition";
            return false;
        }
    }

    size_t image_size = (m_total > 0) ? (size_t)m_total : OTA_SIZE_UNKNOWN;
    esp_err_t err = esp_ota_begin(m_updatePartition, image_size, &m_otaHandle);
    if (err != ESP_OK)
    {
        jzError() << "JZOTADriverEsp32: esp_ota_begin failed" << err;
        return false;
    }
    return true;
}

int64_t JZOTADriverEsp32::write(int64_t offset, const JZByteArray &buffer)
{
    if (buffer.isEmpty())
        return 0;

    // ESP-IDF OTA 为顺序写入, offset 由内部维护, 此处忽略。
    esp_err_t err = esp_ota_write(m_otaHandle, buffer.data(), (size_t)buffer.size());
    if (err != ESP_OK)
    {
        jzError() << "JZOTADriverEsp32: esp_ota_write failed" << err;
        return -1;
    }
    m_written += buffer.size();
    return (int64_t)buffer.size();
}

bool JZOTADriverEsp32::finish()
{
    esp_err_t err = esp_ota_end(m_otaHandle);
    if (err != ESP_OK)
    {
        // 固件有问题(截断 / 损坏 / 格式不合法): 校验失败, 不置启动分区。
        // 复位句柄与状态, 旧固件照常启动, 且允许后续重试。
        jzError() << "JZOTADriverEsp32: esp_ota_end failed (bad firmware)" << err;
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
        jzError() << "JZOTADriverEsp32: esp_ota_set_boot_partition failed" << err;
        m_otaHandle = 0;
        m_updatePartition = nullptr;
        m_total = 0;
        m_written = 0;
        return false;
    }
    m_otaHandle = 0;
    return true;
}

void JZOTADriverEsp32::reboot()
{
    jzDebug() << "JZOTADriverEsp32: restart to apply new firmware";
    esp_restart();
}

void JZOTADriverEsp32::markValid()
{
    // 仅应在新固件已确认可正常运行(如业务初始化 / 网络就绪)后调用。
    // 调用前若崩溃重启, bootloader 会回退到上一有效版本(需开启回滚配置)。
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
        jzWarning() << "JZOTADriverEsp32: mark_app_valid_cancel_rollback failed" << err;
}

void JZOTADriverEsp32::cancel()
{
    // 释放 OTA 句柄(esp_ota_end 会回收句柄资源, 即使镜像不完整校验失败也无妨)。
    // 关键点: 不调用 esp_ota_set_boot_partition, otadata 仍指向旧固件, 不影响启动。
    if (m_otaHandle != 0)
    {
        esp_err_t err = esp_ota_end(m_otaHandle);
        if (err != ESP_OK)
            jzDebug() << "JZOTADriverEsp32: esp_ota_end in cancel (expected if partial)" << err;
        m_otaHandle = 0;
    }
    m_updatePartition = nullptr;
    m_total = 0;
    m_written = 0;
}
