#ifndef JZ_OTA_DRIVER_H_
#define JZ_OTA_DRIVER_H_

#include "JZStd.h"
#include "JZByteArray.h"
#include <cstdint>
#include <string>

// 升级信息(由服务端 manifest 提供, 服务端一定支持 HTTP Range)
struct JZOTAUpdateInfo {
    // ---------- 标识 / 版本 ----------
    std::string version;     // 目标固件版本号
    std::string buildId;     // 构建号 / commit hash
    std::string channel;     // 发布通道: stable / beta / dev

    // ---------- 设备匹配(防刷错固件, 由 driver 决定如何校验) ----------
    std::string productId;   // 产品 / 型号 id, 不匹配应拒绝升级
    std::string hwVersion;   // 目标硬件版本, 不兼容应拒绝升级
    std::string minVersion;  // 允许升级的最低当前版本(防跨版本不兼容)

    // ---------- 下载 ----------
    std::string url;         // 主下载地址
    std::string mirror;      // 备用镜像地址(主地址失败可回退)
    int64_t size = 0;        // 固件总大小(字节), <=0 表示未知(此时整体下载)

    // ---------- 完整性 / 安全 ----------
    std::string md5;         // md5(hex), 写入后校验
    std::string sha256;      // 可选, 更强完整性校验
    std::string signature;   // 可选, 服务端签名(驱动可验签)
    std::string signAlgo;    // 签名算法: "rsa" / "ecdsa" / ...

    // ---------- 升级策略(供上层调度 / UI 使用) ----------
    bool mandatory = false;  // 是否强制升级
    bool reboot = true;      // 完成后是否需要重启
    std::string applyMode;   // 应用时机: "now" / "nextBoot"
    std::string releaseNote; // 更新说明 / changelog
    std::string publishTime; // 发布时间(ISO8601)

    // ---------- 源 ----------
    std::string infoUrl;     // 本 manifest 自身地址(便于回显 / 审计)
};

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

    // 校验升级信息: 返回 true 表示需要执行升级, false 表示无需升级(如已是最新)
    virtual bool verify(const JZOTAUpdateInfo &info) = 0;

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
    // 由 JZOTA 在 finish() 成功后、依据 info.reboot 策略决定是否调用。
    virtual void reboot() = 0;

    // 确认"当前正在运行的固件"有效(仅 ESP32 等支持回滚的平台有意义)。
    // 应用应在自身初始化 / 自检成功后才调用; 调用前若发生崩溃重启,
    // bootloader 会自动回退到上一有效版本。默认空实现(不支持回滚的平台无需处理)。
    virtual void markValid() {}

protected:
    JZ_DISABLE_COPY(JZOTADriver)
};

#endif // !JZ_OTA_DRIVER_H_
