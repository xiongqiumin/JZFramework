#include "JZCrashReport.h"

#include <string>
#include <cstring>
#include <cstdio>
#include "esp_panic.h"     // esp_register_panic_handler / esp_panic_default_handler / panic_info_t
#include "esp_attr.h"      // IRAM_ATTR
#include "esp_err.h"       // ESP_OK
#include "esp_backtrace.h" // esp_backtrace_get / esp_backtrace_frame_t

// 复位后不丢失的 RTC 内存(软件复位保留, 掉电清空)。
// 用于在上电后读取上次崩溃信息并上报。
#define JZ_CRASH_REPORT_MAGIC     0x4A5A4352u   // "JZCR"
#define JZ_CRASH_REPORT_BT_DEPTH  16

struct JZCrashReportEsp32Record
{
    uint32_t magic;
    uint32_t core;
    uint32_t exception;
    uint32_t btDepth;
    uint32_t bt[JZ_CRASH_REPORT_BT_DEPTH]; // 原始返回地址, 可离线 addr2line
    char     reason[128];
};
static RTC_NOINIT_ATTR JZCrashReportEsp32Record s_crashRecord = {0};

// 平台内部实现(与 Windows/Linux 分支对称)
class JZCrashReportImpl
{
public:
    JZCrashCallback callback = nullptr;
};

// 自定义 panic 回调。
// 注意: 此函数在 panic 上下文执行(中断关闭、部分场景 cache 已关闭),
// 因此只做"轻量、IRAM 安全"的操作: 把崩溃签名写入 RTC_NOINIT 内存,
// 切勿在其中做动态分配 / 网络收发等重操作。
// 完整回溯由 esp_panic_default_handler 打印到 UART,
// 因此 idf.py monitor 依然能看到和原来一致的崩溃信息。
static void IRAM_ATTR jzCrashReportPanicHandler(void *info)
{
    auto *impl = JZCrashReportManager::instance()->d;
    auto *pi = static_cast<panic_info_t *>(info);

    // 轻量写入 RTC 内存, 供上电后 takePendingReport() 读取。
    memset(&s_crashRecord, 0, sizeof(s_crashRecord));
    s_crashRecord.magic = JZ_CRASH_REPORT_MAGIC;
    if (pi)
    {
        s_crashRecord.core = (uint32_t)pi->core;
        s_crashRecord.exception = (uint32_t)pi->exception;
        if (pi->reason)
            strncpy(s_crashRecord.reason, pi->reason, sizeof(s_crashRecord.reason) - 1);

        // 抓取原始调用栈返回地址(最佳努力: cache 关闭时可能失败, 失败则 btDepth=0)。
        if (pi->frame)
        {
            esp_backtrace_frame_t frames[JZ_CRASH_REPORT_BT_DEPTH];
            uint32_t depth = 0;
            if (esp_backtrace_get(pi->frame, frames, JZ_CRASH_REPORT_BT_DEPTH, &depth) == ESP_OK)
            {
                s_crashRecord.btDepth = depth;
                for (uint32_t i = 0; i < depth; i++)
                    s_crashRecord.bt[i] = frames[i].pc;
            }
        }
    }
    else
    {
        strncpy(s_crashRecord.reason, "unknown panic", sizeof(s_crashRecord.reason) - 1);
    }

    // 仍在 panic 上下文, 仅允许轻量回调(例如置标志位), 不要在里面做网络/存储。
    if (impl && impl->callback)
    {
        JZCrashReport rpt;
        rpt.platform = "esp32";
        rpt.signal = pi ? (int)pi->exception : 0;
        if (pi && pi->reason) rpt.assert = pi->reason;
        rpt.msg = "ESP32 panic: core " + std::to_string(s_crashRecord.core) +
                  " exception " + std::to_string(s_crashRecord.exception);
        rpt.stack = "(full backtrace dumped to UART by default handler)";
        impl->callback(rpt);
    }

    // 调用默认 panic 处理: 打印完整回溯到 UART, 随后系统复位。
    esp_panic_default_handler(info);
}

// 上电后调用: 读取并清空上次复位前的崩溃记录。
JZOptional<JZCrashReport> JZCrashReportManager::takePendingReport()
{
    if (s_crashRecord.magic != JZ_CRASH_REPORT_MAGIC)
        return JZOptional<JZCrashReport>();   // 无待上报崩溃

    JZCrashReport rpt;
    rpt.platform = "esp32";
    rpt.signal = (int)s_crashRecord.exception;
    rpt.assert = s_crashRecord.reason;
    rpt.msg = "ESP32 panic: core " + std::to_string(s_crashRecord.core) +
              " exception " + std::to_string(s_crashRecord.exception);

    if (s_crashRecord.btDepth > 0)
    {
        std::string bt;
        for (uint32_t i = 0; i < s_crashRecord.btDepth; i++)
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "0x%08x", s_crashRecord.bt[i]);
            bt += buf;
            bt += "\n";
        }
        rpt.stack = bt;
    }
    else
    {
        rpt.stack = "(no backtrace captured at panic time)";
    }

    // 清除标志, 避免重复上报。
    s_crashRecord.magic = 0;
    return JZOptional<JZCrashReport>::Some(std::move(rpt));
}

// ==================== 管理类(分平台实现) ====================
JZCrashReportManager::JZCrashReportManager()
{
    d = new JZCrashReportImpl();
}

JZCrashReportManager::~JZCrashReportManager()
{
    delete d;
}

JZCrashReportManager *JZCrashReportManager::instance()
{
    static JZCrashReportManager inst;
    return &inst;
}

void JZCrashReportManager::init(JZCrashCallback callback)
{
    d->callback = callback;
    // 注册自定义 panic 处理函数。仅能注册一次, 重复注册会返回 ESP_ERR_INVALID_STATE。
    esp_register_panic_handler(&jzCrashReportPanicHandler);
}
