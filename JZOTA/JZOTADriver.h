#ifndef JZ_OTA_DRIVER_H_
#define JZ_OTA_DRIVER_H_

#include "JZStd.h"
#include "JZByteArray.h"
#include "JZResult.h"
#include <cstdint>
#include <string>

// 升级信息结构体定义见 JZOTA.h
struct JZOTAUpdateInfo;

/*
 * JZOTADriver: OTA 写入抽象层
 * 把"下载后的固件数据写到哪里 / 怎么写"这一行为与下载逻辑解耦。
 * JZOTA 只负责下载与调度, 真正的落盘(文件 / Flash / 自定义存储)及校验由具体驱动实现。
 */
class JZOTADriver
{
public:
    JZOTADriver() = default;
    virtual ~JZOTADriver() = default;

    // 全局驱动实例: 在 JZDriver::init 中按平台安装。
    static void setInstance(JZOTADriver *inst);
    static JZOTADriver *instance();


    // 校验能否升级: Ok()=环境就绪(分区存在且容量足够), Error=校验失败
    virtual JZResult<void> verify(const JZOTAUpdateInfo &info) = 0;

    // 期望写入的总字节数(用于进度计算)。未知返回 -1
    virtual int64_t totalSize() = 0;

    // 准备写入目标(打开文件 / 擦除 Flash 等)。失败返回 false
    virtual bool begin(int64_t totalSize) = 0;

    // 在指定绝对偏移处写入一块数据。返回实际写入字节数, < 0 表示错误
    virtual int64_t write(int64_t offset, const JZByteArray &buffer) = 0;

    // 结束写入(关闭文件 / flush / 提交 / 完整性校验)。成功返回 true
    virtual bool finish() = 0;

    // 取消写入: 中止当前升级并清理半成品(关闭句柄 / 删除残文件), 不影响设备启动。
    // 与 finish() 不同, cancel() 不会提交(不会置启动分区 / 不会翻转 boot)。
    virtual void cancel() = 0;

    // 应用并重启以生效新固件(机制由具体驱动实现: 置 boot 标志 + 重启 / 主机场景可为空操作)。
    // 由 JZOTA 在 finish() 成功后、依据 info.extra["reboot"] 策略决定是否调用。
    virtual void reboot() = 0;

    // 确认"当前正在运行的固件"有效(仅 ESP32 等支持回滚的平台有意义)。
    // 应用应在自身初始化 / 自检成功后才调用; 调用前若发生崩溃重启,
    // bootloader 会自动回退到上一有效版本。默认空实现(不支持回滚的平台无需处理)。
    virtual void markValid() = 0;

    // 当前启动是否为 OTA 升级后的首次启动(固件未经验证 / 处于回滚观察期)。
    // ESP32: 运行分区状态 == ESP_OTA_IMG_PENDING_VERIFY 时返回 true。
    // 其他平台默认 false。
    virtual bool isOTAReboot() = 0;

    // 主动回滚: 标记当前固件为无效, 回退到上一有效版本并重启。
    // 仅 ESP32 等支持回滚的平台有意义; 默认空实现。
    // 调用后设备会重启, 不会再返回。
    virtual void rollback() = 0;

protected:
    JZ_DISABLE_COPY(JZOTADriver)

    static JZOTADriver *m_inst;
};

#endif // !JZ_OTA_DRIVER_H_
