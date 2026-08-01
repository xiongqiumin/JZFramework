#ifndef JZ_OTA_DRIVER_ESP32_H_
#define JZ_OTA_DRIVER_ESP32_H_

#include "JZOTADriver.h"
#include "JZOTA.h"
#include "JZByteArray.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

/*
 * JZOTADriverEsp32: ESP32 (ESP-IDF) 平台下的 OTA 写入驱动
 *
 * 基于 ESP-IDF 的 OTA 分区机制:
 *   verify  -> 选定下一个 OTA 更新分区, 并校验空间是否足够
 *   begin   -> esp_ota_begin 打开写句柄(顺序写入)
 *   write   -> esp_ota_write 顺序写入固件数据(offset 由 ESP-IDF 内部维护, 此处忽略)
 *   finish  -> esp_ota_end 校验镜像 + esp_ota_set_boot_partition 置为可启动
 *   reboot  -> esp_restart 重启以生效新固件
 *
 * 注: 镜像完整性由 ESP-IDF 在 esp_ota_end 内校验(配置了 SHA-256 / 安全启动时),
 *     因此本驱动不再单独做 md5 回读校验(避免整片 Flash 回读)。
 */
class JZOTADriverEsp32 : public JZOTADriver
{
public:
    JZOTADriverEsp32();
    ~JZOTADriverEsp32() override;

    // 校验能否升级: 存在可用 OTA 分区且容量足够则返回 Ok()
    JZResult<void> verify(const JZOTAUpdateInfo &info) override;

    // 期望写入的总字节数(用于进度计算)
    int64_t totalSize() override;

    // 准备写入目标分区
    bool begin(int64_t totalSize) override;

    // 写入一块固件数据(顺序写入, offset 被忽略)
    int64_t write(int64_t offset, const JZByteArray &buffer) override;

    // 结束写入: 校验镜像并置为下次启动分区
    bool finish() override;

    // 取消写入: 释放 OTA 句柄(不置启动分区, 不影响设备启动)
    void cancel() override;

    // 重启以生效新固件
    void reboot() override;

    // 确认当前运行固件有效(防回滚: 调用前崩溃则下次启动回退旧版本)
    void markValid() override;

    // 当前启动是否为 OTA 升级后的首次启动(PENDING_VERIFY 状态)
    bool isOTAReboot() override;

    // 主动回滚: 标记当前固件无效, 回退上一版本并重启
    void rollback() override;

private:
    const esp_partition_t *m_updatePartition = nullptr;
    esp_ota_handle_t m_otaHandle = 0;
    int64_t m_total = 0;
    int64_t m_written = 0;
};

#endif // !JZ_OTA_DRIVER_ESP32_H_
