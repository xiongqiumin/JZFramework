#include "JZOTADriverEsp32.h"
#include "JZDebug.h"
#include "esp_system.h"

JZOTADriverEsp32::JZOTADriverEsp32()
{
}

JZOTADriverEsp32::~JZOTADriverEsp32()
{
}

JZResult<void> JZOTADriverEsp32::verify(const JZOTAUpdateInfo &info)
{
    m_updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (!m_updatePartition)
    {
        jzError() << "JZOTADriverEsp32: no OTA update partition found";
        return JZResult<void>::Error("no OTA update partition found");
    }

    // 容量检查: 已知大小且超出分区则拒绝
    if (info.size > 0 && (int64_t)m_updatePartition->size < info.size)
    {
        jzError() << "JZOTADriverEsp32: firmware too large" << info.size
                  << " > partition" << m_updatePartition->size;
        return JZResult<void>::Error("firmware too large for partition");
    }

    jzDebug() << "JZOTADriverEsp32: update partition=" << m_updatePartition->label
              << " size=" << m_updatePartition->size;    

    return JZResult<void>::Ok();
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

    esp_err_t err = esp_ota_begin(m_updatePartition, (size_t)m_total, &m_otaHandle);
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

bool JZOTADriverEsp32::isOTAReboot()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK)
        return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

void JZOTADriverEsp32::rollback()
{
    jzDebug() << "JZOTADriverEsp32: rollback to previous firmware";
    esp_ota_mark_app_invalid_rollback_and_reboot();
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
