#include "Windows.h"
#include <dbghelp.h>

#include <stdio.h>
#include <string.h>

#include <string>

#include "JZCrashReport.h"

// Windows 崩溃报告:
//   - 解析异常代码/地址, 用 dbghelp 走一遍栈并解析符号(exe 同目录有 pdb 就有函数名+文件行);
//   - 落一份 minidump 和一份文本日志到 <exe目录>/crash/, 供事后用调试器打开;
//   - 最后回调上层(可再打日志/上报)。
// 注意: 这里运行在崩溃上下文里, 尽量只用 Win32 API, 不做可能再次崩溃的重活。

class JZCrashReportImpl
{
public:
    JZCrashCallback callback = nullptr;
};

// 定义在文件末尾; 异常过滤器要先声明
void JZCrashReportSignalHandler(int sig);

static EXCEPTION_POINTERS *g_exception = nullptr;

static std::string exeDir()
{
    char path[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return std::string(".");
    std::string p(path, n);
    size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

static std::string crashDir()
{
    std::string dir = exeDir() + "\\crash";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

static std::string makeName(const char *tag, const char *ext)
{
    char path[MAX_PATH] = { 0 };
    snprintf(path, sizeof(path), "%s\\%s_%lu%s", crashDir().c_str(), tag,
             (unsigned long)GetCurrentProcessId(), ext);
    return std::string(path);
}

// DbgHelp 不可用时的退路: 只给 [模块名+偏移], 事后用 pdb/dumpbin 定位。
// 典型场景: 进程正在退出 (main 已返回), 这时 SymInitialize 会以 87
// (ERROR_INVALID_PARAMETER) 失败 —— 但地址本身仍然有意义。
static std::string rawStack()
{
    void *frames[32];
    const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);

    std::string out;
    char line[512];
    for (USHORT i = 0; i < n; i++)
    {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        unsigned long long off = (unsigned long long)frames[i];
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &mod) && mod)
        {
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char *base = strrchr(name, '\\');
            if (base)
                memmove(name, base + 1, strlen(base + 1) + 1);
            off = (unsigned long long)frames[i] - (unsigned long long)mod;
        }
        snprintf(line, sizeof(line), "#%-2d %s+0x%llX\n", i, name, off);
        out += line;
    }
    return out;
}

// 逐个栈帧: 地址 + 符号名(有 pdb 时) + 源码位置(有行信息时)
static std::string buildStack(EXCEPTION_POINTERS *info)
{
    if (!info || !info->ContextRecord)
        return std::string();

    // ⚠ 用**真实进程句柄**而不是 GetCurrentProcess() 伪句柄: 伪句柄 (-1) + fInvadeProcess=TRUE
    //   在部分 dbghelp 版本下会直接返回 ERROR_INVALID_PARAMETER(87), 于是整段堆栈丢失。
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                                 GetCurrentProcessId());
    if (!process)
        process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // 第二个参数为额外的符号搜索路径; pdb 就在 exe 同目录, 默认模块路径即可找到
    if (!SymInitialize(process, nullptr, TRUE))
    {
        const DWORD err = GetLastError();
        return "SymInitialize failed: " + std::to_string(err) + "\n" + rawStack();
    }

    CONTEXT ctx = *info->ContextRecord;
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));

    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    std::string out;
    char line[768];

    for (int i = 0; i < 32; i++)
    {
        if (!StackWalk64(machine, process, thread, &frame, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;

        DWORD64 addr = frame.AddrPC.Offset;
        if (addr == 0)
            break;

        char symbolBuf[sizeof(SYMBOL_INFO) + 256] = { 0 };
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbolBuf;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;

        DWORD64 disp = 0;
        const char *name = "?";
        if (SymFromAddr(process, addr, &disp, symbol))
            name = symbol->Name;

        IMAGEHLP_LINE64 srcLine;
        memset(&srcLine, 0, sizeof(srcLine));
        srcLine.SizeOfStruct = sizeof(srcLine);
        DWORD lineDisp = 0;

        if (SymGetLineFromAddr64(process, addr, &lineDisp, &srcLine))
        {
            const char *file = srcLine.FileName ? srcLine.FileName : "?";
            const char *base = strrchr(file, '\\');
            if (base)
                file = base + 1;
            snprintf(line, sizeof(line), "#%-2d 0x%016llX %s+0x%llX  (%s:%lu)\n",
                     i, (unsigned long long)addr, name, (unsigned long long)disp,
                     file, (unsigned long)srcLine.LineNumber);
        }
        else
        {
            snprintf(line, sizeof(line), "#%-2d 0x%016llX %s+0x%llX\n",
                     i, (unsigned long long)addr, name, (unsigned long long)disp);
        }
        out += line;
    }

    SymCleanup(process);
    if (process != GetCurrentProcess())
        CloseHandle(process);
    return out;
}

static bool writeAll(const std::string &path, const char *data, int size)
{
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    BOOL ok = WriteFile(file, data, (DWORD)size, &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && (int)written == size;
}

static std::string writeDump(EXCEPTION_POINTERS *info)
{
    std::string path = makeName("crash", ".dmp");
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::string();

    MINIDUMP_EXCEPTION_INFORMATION info2;
    info2.ThreadId = GetCurrentThreadId();
    info2.ExceptionPointers = info;
    info2.ClientPointers = FALSE;

    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo);
    bool ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                info ? &info2 : nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(file);

    return ok ? path : std::string();
}

// 异常过滤: 先记下 EXCEPTION_POINTERS, 再走统一入口(那里能访问 manager 的私有实现)
static LONG WINAPI Win32ExceptionHandler(EXCEPTION_POINTERS *exceptionInfo)
{
    static volatile LONG entered = 0;
    if (InterlockedExchange(&entered, 1) != 0)
        return EXCEPTION_EXECUTE_HANDLER;   // 处理过程中再次崩溃: 直接放行

    g_exception = exceptionInfo;
    JZCrashReportSignalHandler(0);
    return EXCEPTION_EXECUTE_HANDLER;
}

void JZCrashReportSignalHandler(int sig)
{
    (void)sig;

    EXCEPTION_POINTERS *info = g_exception;
    EXCEPTION_RECORD *rec = (info && info->ExceptionRecord) ? info->ExceptionRecord : nullptr;

    JZCrashReport report;
    report.platform = "windows";
    report.signal = rec ? (int)rec->ExceptionCode : 0;

    char head[256] = { 0 };
    snprintf(head, sizeof(head), "code=0x%08X addr=%p thread=%lu",
             rec ? (unsigned int)rec->ExceptionCode : 0,
             rec ? (void *)rec->ExceptionAddress : nullptr,
             (unsigned long)GetCurrentThreadId());
    report.msg = head;
    report.stack = buildStack(info);

    std::string dump = info ? writeDump(info) : std::string();

    std::string text = std::string("=== crash ===\n") + head + "\n" + report.stack;
    if (!dump.empty())
        text += "dump: " + dump + "\n";
    text += "=============\n";
    writeAll(makeName("crash", ".log"), text.data(), (int)text.size());

    auto d = JZCrashReportManager::instance()->d;
    if (d && d->callback)
        d->callback(report);
}

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
    SetUnhandledExceptionFilter(Win32ExceptionHandler);
}

JZOptional<JZCrashReport> JZCrashReportManager::takePendingReport()
{
    return JZOptional<JZCrashReport>();   // Windows 平台无复位持久化崩溃记录
}
