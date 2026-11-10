#include "JZProcess.h"
#include "JZDebug.h"
#include "JZElapsedTimer.h"
#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZMutex.h"
#include "JZStd.h"      // str_to_wstr: UTF-8 -> UTF-16(命令行必须走宽字符 API)
#include "JZTx.h"

using namespace std;

// ==================== Windows 平台 ====================
#ifdef _WIN32

#include <Windows.h>

// Windows 命令行参数引号规则(与 CommandLineToArgvW 一致):
// 参数含空格/制表符/双引号时必须整体加引号; 参数内的双引号转义为 \", 其前的反斜杠要翻倍。
// 旧实现只"含空格就加引号"但不转义内层引号, 导致 `bash -c "python -c \"x\""` 这类
// 嵌套引号被 CreateProcess 解析错位(典型表现: 经 shell 执行的命令里内层引号被吃掉)。
static string winQuoteArg(const string &arg)
{
    bool needQuote = false;
    for (char c : arg)
    {
        if (c == ' ' || c == '\t' || c == '"')
        {
            needQuote = true;
            break;
        }
    }
    if (!needQuote)
        return arg;

    string out = "\"";
    int backslashes = 0;
    for (char c : arg)
    {
        if (c == '\\')
        {
            backslashes++;
        }
        else if (c == '"')
        {
            out.append((size_t)backslashes * 2, '\\');   // 翻倍前面的反斜杠
            out += "\\\"";                                // 再转义这个引号
            backslashes = 0;
        }
        else
        {
            out.append((size_t)backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append((size_t)backslashes * 2, '\\');           // 结尾的反斜杠翻倍
    out += "\"";
    return out;
}

static bool startDetachedProcess(const string &program, const vector<string> &arguments, const string &workingDirectory)
{
    // 与 createProcess 相同的引号规则(见 winQuoteArg)
    string programToken = program;
    if (str_contains(programToken, " "))
        programToken = "\"" + programToken + "\"";
    string cmdLine = programToken;
    for (const auto &arg : arguments)
        cmdLine += " " + winQuoteArg(arg);

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);

    // 分离进程: 不创建管道, bInheritHandles = FALSE
    const char *appName = JZDir::isAbsolutePath(program) ? program.c_str() : nullptr;
    BOOL success;
    if (!workingDirectory.empty())
    {
        string work_dir = workingDirectory;
        str_replace(work_dir, "/", "\\");
        success = CreateProcessA(appName, const_cast<char *>(cmdLine.c_str()), nullptr, nullptr, FALSE, 0, nullptr,
                                 work_dir.c_str(), &si, &pi);
    }
    else
    {
        success = CreateProcessA(appName, const_cast<char *>(cmdLine.c_str()), nullptr, nullptr, FALSE, 0, nullptr,
                                 nullptr, &si, &pi);
    }

    if (!success)
    {
        jzError() << "startDetached: CreateProcess failed. program:" << program
                  << " wd:" << workingDirectory << " err:" << (int)GetLastError();
        return false;
    }

    // 不保留句柄, 进程句柄随父子进程解绑
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// 把 CreateProcess 的失败码翻成"能指导下一步"的中文。
// ⚠ 以前调用方只拿到 bool，就把所有失败都读成"程序不存在"；实测最常见的是工作目录不存在
// （ERROR_DIRECTORY 267 —— 如 run_js 在还没编译、build/ 尚未生成时启动）。
static string describeStartError(DWORD err, const string &program, const string &workingDirectory)
{
    switch (err)
    {
    case ERROR_DIRECTORY:          // 267: 目录名无效（工作目录不存在 / 不是目录）
        return "工作目录不存在或不是目录: " + workingDirectory;
    case ERROR_FILE_NOT_FOUND:     // 2
    case ERROR_PATH_NOT_FOUND:     // 3
        return "找不到程序: " + program;
    case ERROR_ACCESS_DENIED:      // 5
        return "拒绝访问（无执行权限 / 被安全策略或杀软拦截）: " + program;
    default:
        return "CreateProcess 失败（错误码 " + to_string((int)err) + "）: " + program
               + (workingDirectory.empty() ? string() : ("  wd=" + workingDirectory));
    }
}

class JZProcessPrivate
{
public:
    JZProcessPrivate(JZProcess *q);
    ~JZProcessPrivate();
    
    bool createProcess();
    void closeHandles();
    void onReadTimer();
    bool waitForFinished(int msecs);
    int64_t write(const JZByteArray &data);
    void terminate();
    void killProcess();
    void closeWriteChannel();

    void normal(JZByteArray &buffer);
    void processRead();
    bool tryWaitForExit();

    JZProcess *q_ptr;
    string program;
    vector<string> arguments;
    string workingDirectory;
    JZProcess::ProcessChannelMode channelMode;
    JZProcess::InputChannelMode inputMode = JZProcess::ManagedInputChannel;
    JZProcess::OutputChannelMode outputMode = JZProcess::ManagedOutputChannel;
    JZProcess::ProcessState state;
    int exitCode;
    JZProcess::ExitStatus exitStatus;
    bool m_terminateRequested;
    // start() 失败原因（由 describeStartError 归类），供 JZProcess::lastStartError() 取用
    string startError;

    HANDLE hProcess;
    HANDLE hThread;
    HANDLE hStdinWrite;
    char stdOutTail; 
    HANDLE hStdoutRead;
    char stdErrTail;
    HANDLE hStderrRead;
    // 见 setStandardOutputProcess(): 本进程的 stdout 接到谁
    JZProcess *stdoutTarget = nullptr;
    DWORD pid = 0;        // 真实进程号(CreateProcess 填的 dwProcessId); 未启动/已回收为 0
    // 每次启动建的作业对象: 用它才**杀得掉整棵进程树**(见 terminate)。
    // 只 TerminateProcess 只会杀到我们自己起的那个进程(通常是 shell), 它的子进程
    // (真正的服务 / sleep / node ...)会活下来继续占端口。
    HANDLE hJob = INVALID_HANDLE_VALUE;

    JZByteArray stdoutBuffer;
    JZByteArray stderrBuffer;
    // 定时器线程和 waitForFinished() 所在的调用线程都会读管道 / 改上面两个缓冲区
    mutable JZMutex mutex;
};

JZProcessPrivate::JZProcessPrivate(JZProcess *q)
    : q_ptr(q),
      channelMode(JZProcess::SeparateChannels),
      state(JZProcess::NotRunning),
      exitCode(0),
      exitStatus(JZProcess::NormalExit),
      m_terminateRequested(false),
      hProcess(INVALID_HANDLE_VALUE),
      hThread(INVALID_HANDLE_VALUE),
      hStdinWrite(INVALID_HANDLE_VALUE),
      hStdoutRead(INVALID_HANDLE_VALUE),
      hStderrRead(INVALID_HANDLE_VALUE),
      hJob(INVALID_HANDLE_VALUE)
{
    stdOutTail = 0;
    stdErrTail = 0;
}

JZProcessPrivate::~JZProcessPrivate()
{
    closeHandles();
}

void JZProcessPrivate::closeHandles()
{
    if (hStdinWrite != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStdinWrite);
        hStdinWrite = INVALID_HANDLE_VALUE;
    }
    if (hStdoutRead != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStdoutRead);
        hStdoutRead = INVALID_HANDLE_VALUE;
    }
    if (hStderrRead != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hStderrRead);
        hStderrRead = INVALID_HANDLE_VALUE;
    }
    if (hProcess != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hProcess);
        hProcess = INVALID_HANDLE_VALUE;
    }
    if (hThread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hThread);
        hThread = INVALID_HANDLE_VALUE;
    }
    // 关作业对象: 带 KILL_ON_JOB_CLOSE, 所以这里是"最后一次清理子孙进程"的机会
    // (进程正常跑完时作业里早没活了, 不会误杀)。
    if (hJob != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hJob);
        hJob = INVALID_HANDLE_VALUE;
    }
}

bool JZProcessPrivate::createProcess()
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE hStdinRead = nullptr, hStdoutWrite = nullptr, hStderrWrite = nullptr;

    // 上游进程(`setStandardOutputProcess(下游)`): 标准输出直接写进下游的 stdin 管道,
    // 自己不再建 stdout 管道 —— 两个进程真正并发, 数据在管道里流动, 不经过我们的内存。
    JZProcessPrivate *targetD = (stdoutTarget != nullptr) ? stdoutTarget->d_ptr : nullptr;
    if (targetD != nullptr && targetD->hStdinWrite == INVALID_HANDLE_VALUE)
    {
        // 下游的 stdin 写端必须还在(它已经 closeWriteChannel 或没启动时就连不上)
        startError = "下游进程还没有启动（先 start 下游，再 start 上游）";
        return false;
    }

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0))
    {
        startError = "创建管道失败（错误码 " + to_string((int)GetLastError()) + "）";
        return false;
    }
    // 继承模式: 不建 stdout/stderr 管道 —— 子进程直接写**我们自己的** fd 1 / fd 2
    const bool inheritOut = (outputMode == JZProcess::InheritOutputChannel);
    if (targetD == nullptr && !inheritOut && !CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0))
    {
        CloseHandle(hStdinRead);
        CloseHandle(hStdinWrite);
        hStdinRead = hStdinWrite = INVALID_HANDLE_VALUE;
        startError = "创建管道失败（错误码 " + to_string((int)GetLastError()) + "）";
        return false;
    }

    if (channelMode == JZProcess::SeparateChannels && !inheritOut)
    {
        if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0))
        {
            CloseHandle(hStdinRead);
            CloseHandle(hStdinWrite);
            if (hStdoutRead != INVALID_HANDLE_VALUE)
                CloseHandle(hStdoutRead);
            if (hStdoutWrite != nullptr)
                CloseHandle(hStdoutWrite);
            hStdinRead = hStdinWrite = hStdoutRead = hStdoutWrite = INVALID_HANDLE_VALUE;
            startError = "创建 stderr 管道失败（错误码 " + to_string((int)GetLastError()) + "）";
            return false;
        }
    }

    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    if (hStdoutRead != INVALID_HANDLE_VALUE)
        SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    if (channelMode == JZProcess::SeparateChannels)
    {
        SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFOW));
    si.cb = sizeof(STARTUPINFOW);
    // 继承模式下子进程直接用**我们自己的** stdin(脚本从 stdin 读进来时, 里面起的命令
    // 要接着读剩下那部分 —— bash 的行为)
    si.hStdInput = (inputMode == JZProcess::InheritInputChannel)
                       ? GetStdHandle(STD_INPUT_HANDLE)
                       : hStdinRead;
    // 接了上游: 我们的 stdout 就是下游 stdin 管道的**写端**(子进程继承一份;
    // 启动后父进程再把自己的那份关掉, 这样上游一退出下游立刻收到 EOF)
    if (targetD != nullptr)
    {
        // 下游启动时把它的 stdin 写端设成"不继承"了(怕别的子进程继承),
        // 这里要放开, 否则上游的子进程根本拿不到这个句柄。
        SetHandleInformation(targetD->hStdinWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    HANDLE hStdoutToChild = (targetD != nullptr) ? targetD->hStdinWrite
                            : (inheritOut ? GetStdHandle(STD_OUTPUT_HANDLE) : hStdoutWrite);
    si.hStdOutput = hStdoutToChild;
    if (inheritOut)
    {
        // 继承给子进程的句柄必须带 HANDLE_FLAG_INHERIT, 否则 CreateProcess 不会转交,
        // 子进程拿到的 fd 1/2 就是空的。
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr)
            SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        if (hErr != INVALID_HANDLE_VALUE && hErr != nullptr)
            SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.hStdError = hErr;
    }
    else
    {
        si.hStdError = (channelMode == JZProcess::SeparateChannels) ? hStderrWrite : hStdoutToChild;
    }
    si.dwFlags |= STARTF_USESTDHANDLES;

    if (!JZDir::isAbsolutePath(program) && !workingDirectory.empty())
    {
        string program_name = program;
        string basePath = workingDirectory + "/";
        JZFileInfo pro_info(program_name);
        string fullPath = basePath + program_name;
        if (JZFile::exists(fullPath))
        {
            program = fullPath;
        }
        else
        {
            // 检查是否需要添加后缀
            JZFileInfo pro_info(program_name);
            bool hasExtension = !pro_info.suffix().empty();

            if (!hasExtension)
            {
                // 尝试添加 .exe
                string exePath = basePath + program_name + ".exe";
                if (JZFile::exists(exePath))
                {
                    program = exePath;
                }
                else
                {
                    // 尝试添加 .bat
                    string batPath = basePath + program_name + ".bat";
                    if (JZFile::exists(batPath))
                    {
                        program = batPath;
                    }
                }
            }
        }
    }

    // 程序路径若含空格必须加引号, 否则 CreateProcess 解析命令行首 token 会被截断,
    // 也容易触发带全局 CreateProcess Hook 的第三方模块(如 MozartBreathCore.dll)崩溃
    string programToken = program;
    if (str_contains(programToken, " "))
        programToken = "\"" + programToken + "\"";
    string cmdLine = programToken;
    for (const auto &arg : arguments)
        cmdLine += " " + winQuoteArg(arg);

    BOOL success;
    // 传入绝对路径作为 lpApplicationName, 可绕过部分 Hook 对命令行首 token 的重新解析
    const bool hasAppName = JZDir::isAbsolutePath(program);
    // ⚠ 命令行必须走 **宽字符** API: CreateProcessA 会把 UTF-8 字节按 ANSI(GBK) 解释,
    //   中文参数/中文文件名直接变乱码(实测 `grep 中文` 匹配不到、`touch 中文.txt` 建出乱码文件名)。
    std::wstring wCmdLine = str_to_wstr(cmdLine);
    std::wstring wAppName = hasAppName ? str_to_wstr(program) : std::wstring();
    std::wstring wWorkDir;
    if (!workingDirectory.empty())
    {
        string work_dir = workingDirectory;
        str_replace(work_dir, "/", "\\");
        wWorkDir = str_to_wstr(work_dir);
    }
    success = CreateProcessW(hasAppName ? wAppName.c_str() : nullptr,
                             wCmdLine.empty() ? nullptr : &wCmdLine[0],
                             nullptr, nullptr, TRUE, 0, nullptr,
                             wWorkDir.empty() ? nullptr : wWorkDir.c_str(), &si, &pi);

    CloseHandle(hStdinRead);
    hStdinRead = INVALID_HANDLE_VALUE;
    if (inputMode == JZProcess::InheritInputChannel)
    {
        // 继承模式不用我们喂 stdin: 关掉写端(write() 变成 no-op, 子进程直接读我们的 stdin)
        CloseHandle(hStdinWrite);
        hStdinWrite = INVALID_HANDLE_VALUE;
    }

    if (hStdoutWrite != nullptr)
    {
        CloseHandle(hStdoutWrite);
        hStdoutWrite = INVALID_HANDLE_VALUE;
    }
    if (channelMode == JZProcess::SeparateChannels && hStderrWrite != nullptr)
    {
        CloseHandle(hStderrWrite);
    }

    if (!success)
    {
        DWORD err = GetLastError();
        startError = describeStartError(err, program, workingDirectory);
        jzError() << "CreateProcess failed. program:" << program << " wd:" << workingDirectory << " err:" << (int)err;
        CloseHandle(hStdinWrite);
        if (hStdoutRead != INVALID_HANDLE_VALUE)
            CloseHandle(hStdoutRead);
        hStdinWrite = hStdoutRead = INVALID_HANDLE_VALUE;
        if (channelMode == JZProcess::SeparateChannels)
        {
            CloseHandle(hStderrRead);
            hStderrRead = INVALID_HANDLE_VALUE;
        }
        return false;
    }

    hProcess = pi.hProcess;
    hThread = pi.hThread;
    pid = pi.dwProcessId;

    // 接了上游: 下游的 stdin 写端必须关掉 —— 否则上游退出后下游等不到 EOF, 会一直读下去。
    if (targetD != nullptr && targetD->hStdinWrite != INVALID_HANDLE_VALUE)
    {
        CloseHandle(targetD->hStdinWrite);
        targetD->hStdinWrite = INVALID_HANDLE_VALUE;
    }

    // 把子进程放进一个作业对象: 以后 TerminateJobObject 就能连它的**整棵进程树**一起收掉
    // (只 TerminateProcess 的话, `bash -c "node server.js"` 里真正占端口的 node 会活下来)。
    // KILL_ON_JOB_CLOSE: 我们(agent)退出/崩溃时, 作业句柄关闭 ⇒ 子孙进程自动被清理,
    // 不会留下占着端口的孤儿。
    // ⚠ 作业是**每次启动**新建的: 一个命令一棵树, 停它不影响别的命令。
    // ⚠ 已有作业里再放作业需要系统支持(嵌套作业): 分配失败就退回"只杀直接子进程"的老行为。
    if (hJob != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hJob);
        hJob = INVALID_HANDLE_VALUE;
    }
    hJob = CreateJobObjectA(nullptr, nullptr);
    if (hJob != INVALID_HANDLE_VALUE)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
        ZeroMemory(&info, sizeof(info));
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
            !AssignProcessToJobObject(hJob, pi.hProcess))
        {
            // 失败(常见于"我们自己已经在别人的作业里且不允许嵌套"): 关掉作业, 退回老行为
            jzWarning() << "JZProcess: 作业对象不可用(进程树收尾退化为只杀直接子进程), err:"
                        << (int)GetLastError();
            CloseHandle(hJob);
            hJob = INVALID_HANDLE_VALUE;
        }
    }

    return true;
}

void JZProcessPrivate::normal(JZByteArray &buffer)
{
    int writePos = 0;
    for (int readPos = 0; readPos < buffer.size(); ++readPos)
    {
        if (buffer[readPos] == '\r' && readPos + 1 < buffer.size() && buffer[readPos + 1] == '\n')
        {
            buffer[writePos++] = '\n';
            readPos++;  // 跳过 \n
        }
        else
        {
            buffer[writePos++] = buffer[readPos];
        }
    }
    buffer.resize(writePos);
}

namespace
{
// 从一个匿名管道里把"现在能读到的"全部读进 buf。返回是否有新数据。
// Windows 匿名管道的 ReadFile 是**阻塞**的: 写端未关闭且当前无数据时会一直等,
// 所以必须先 PeekNamedPipe 探测; Peek 失败且为 ERROR_BROKEN_PIPE 表示写端已关闭,
// 这时 ReadFile 会立即返回剩余数据直到 EOF, 用来排空最后的数据。
bool readPipeChannel(HANDLE h, JZByteArray &buf, char &tail)
{
    if (h == INVALID_HANDLE_VALUE || h == nullptr)
        return false;

    char buffer[4096];
    DWORD bytesRead = 0;

    DWORD avail = 0;
    if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
    {
        if (avail == 0)
            return false;            // 无数据, 跳过(不阻塞)
        DWORD toRead = (avail < sizeof(buffer)) ? avail : (DWORD)sizeof(buffer);
        if (!ReadFile(h, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;
    }
    else
    {
        // 写端已关闭(进程退出): 最后一次 ReadFile 排空剩余, 不会阻塞
        if (GetLastError() != ERROR_BROKEN_PIPE)
            return false;
        if (!ReadFile(h, buffer, (DWORD)sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0)
            return false;
    }

    if (tail != 0)
    {
        buf.push_back(tail);
        tail = 0;
    }
    if (bytesRead > 0 && buffer[bytesRead - 1] == '\r')
    {
        tail = '\r';
        bytesRead--;
    }
    buf.append(buffer, (int)bytesRead);
    // CRLF -> LF(与 JZProcessPrivate::normal 同一套规则); readPipeChannel 是自由函数,
    // 不能再调成员函数, 就地做一遍
    {
        int writePos = 0;
        for (int readPos = 0; readPos < buf.size(); ++readPos)
        {
            if (buf[readPos] == '\r' && readPos + 1 < buf.size() && buf[readPos + 1] == '\n')
            {
                buf[writePos++] = '\n';
                readPos++;
            }
            else
            {
                buf[writePos++] = buf[readPos];
            }
        }
        buf.resize(writePos);
    }
    return true;
}
} // namespace

void JZProcessPrivate::processRead()
{
    bool gotOut = false;
    bool gotErr = false;
    {
        // 定时器(事件线程)和 waitForFinished()(调用方线程)都会读同一个管道,
        // 必须串起来, 否则 ReadFile 会互相抢数据 / 缓冲区被并发改。
        JZMutexLocker lock(&mutex);
        gotOut = readPipeChannel(hStdoutRead, stdoutBuffer, stdOutTail);
        if (channelMode == JZProcess::SeparateChannels)
            gotErr = readPipeChannel(hStderrRead, stderrBuffer, stdErrTail);
    }
    // 信号在**解锁之后**发: 槽里通常直接调 readAllStandardOutput()/bytesAvailable(),
    // 持锁发信号会自己锁自己(不可重入锁会死锁)。
    if (gotOut)
        q_ptr->sigReadyReadStandardOutput.emit();
    if (gotErr)
        q_ptr->sigReadyReadStandardError.emit();
}

void JZProcessPrivate::onReadTimer()
{
    if (state != JZProcess::Running)
        return;

    processRead();
    tryWaitForExit();
}

bool JZProcessPrivate::tryWaitForExit()
{
    DWORD result = WaitForSingleObject(hProcess, 0);
    if (result == WAIT_OBJECT_0) {
        // 进程已退出，可以获取退出代码
        DWORD exitCode = 0;
        GetExitCodeProcess(hProcess, &exitCode);
        
        processRead();

        state = JZProcess::NotRunning;
        closeHandles();
        JZProcess::ExitStatus status = m_terminateRequested ? JZProcess::CrashExit : JZProcess::NormalExit;
        q_ptr->finished(exitCode, status);
        return true;
    }
    else if (result == WAIT_TIMEOUT) {
        // 进程仍在运行，什么都不做
        return false;
    }
    else {
        state = JZProcess::NotRunning;
        closeHandles();
        q_ptr->finished(255, JZProcess::CrashExit);
        return true;
    }
}

bool JZProcessPrivate::waitForFinished(int msecs)
{
    if (state == JZProcess::NotRunning)
        return true;

    // msecs < 0 = 无限等(QProcess 的约定; 以前的实现直接返回 false, 析构里
    // `waitForFinished(-1)` 形同虚设)。
    const bool forever = (msecs < 0);
    JZElapsedTimer t;
    t.start();

    while (forever || t.elapsed() < msecs)
    {
        // ⭐ 等待期间必须继续收数据: 管道的缓冲区(几 KB ~ 几十 KB)写满之后,
        //    子进程会阻塞在 write 上再也退不出来 —— 这就是"输出稍多就卡死"的根因。
        //    以前这里只 tryWaitForExit(), 于是 `cmd | lot-of-output` 必死锁。
        processRead();
        if (tryWaitForExit())
            return true;

        JZThread::msleep(5);
    }
    return false;
}

int64_t JZProcessPrivate::write(const JZByteArray &data)
{
    if (state != JZProcess::Running || hStdinWrite == nullptr)
    {
        return 0;
    }
    DWORD bytesWritten;
    if (!WriteFile(hStdinWrite, data.data(), data.size(), &bytesWritten, nullptr))
    {
        return 0;
    }
    return bytesWritten;
}

void JZProcessPrivate::terminate()
{
    if (state != JZProcess::Running || hProcess == INVALID_HANDLE_VALUE)
    {
        return;
    }
    m_terminateRequested = true;
    // 优先杀**整棵进程树**(作业对象): 只杀直接子进程的话, shell 底下的服务会变成孤儿,
    // 端口不释放 —— 那正是"起了服务收不干净"的根因。
    if (hJob != INVALID_HANDLE_VALUE)
        TerminateJobObject(hJob, 1);
    else
        TerminateProcess(hProcess, 1);
    // 同步等待进程真正退出再上报结束: 异步定时器路径在长时进程下有竞态,
    // 会让 state 长期卡在 Running。最多等 3s, TerminateProcess 必然使进程退出。
    WaitForSingleObject(hProcess, 3000);
    DWORD exitCode = 0;
    GetExitCodeProcess(hProcess, &exitCode);
    state = JZProcess::NotRunning;
    closeHandles();
    q_ptr->finished(exitCode, JZProcess::CrashExit);
}

void JZProcessPrivate::killProcess()
{
    terminate();
}

void JZProcessPrivate::closeWriteChannel()
{
    if (hStdinWrite != nullptr)
    {
        CloseHandle(hStdinWrite);
        hStdinWrite = nullptr;
    }
}

// ==================== Linux 平台 ====================
#elif defined(__linux__)

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <poll.h>
#include <string.h>     // strerror

static bool startDetachedProcess(const string &program, const vector<string> &arguments, const string &workingDirectory)
{
    pid_t pid = ::fork();
    if (pid == -1)
        return false;

    if (pid == 0)
    {
        // 子进程1: 再次 fork 后由孙进程 exec, 子进程1立即退出。
        // 孙进程被 init 收养, 不会成为当前进程的僵尸, 无需 wait 回收。
        pid_t child = ::fork();
        if (child == 0)
        {
            if (!workingDirectory.empty())
            {
                if (::chdir(workingDirectory.c_str()) == -1)
                    _exit(127);
            }

            vector<char *> argv;
            argv.push_back(const_cast<char *>(program.c_str()));
            for (const auto &arg : arguments)
                argv.push_back(const_cast<char *>(arg.c_str()));
            argv.push_back(nullptr);

            ::execvp(program.c_str(), argv.data());
            _exit(127);
        }
        _exit(0);
    }

    // 父进程: 回收子进程1, 孙进程由 init 接管
    int status;
    ::waitpid(pid, &status, 0);
    return true;
}

class JZProcessPrivate
{
public:
    JZProcessPrivate(JZProcess *q);
    ~JZProcessPrivate();

    bool createProcess();
    void closeHandles();
    void onReadTimer();
    bool waitForFinished(int msecs);
    int64_t write(const JZByteArray &data);
    void terminate();
    void killProcess();
    void closeWriteChannel();
    void processRead();
    // 杀整棵进程树(见实现): 子进程 setpgid(0,0) 之后 kill(-pid) 命中整组
    void killTree(int sig);

    JZProcess *q_ptr;
    string program;
    vector<string> arguments;
    string workingDirectory;
    JZProcess::ProcessChannelMode channelMode;
    JZProcess::InputChannelMode inputMode = JZProcess::ManagedInputChannel;
    JZProcess::OutputChannelMode outputMode = JZProcess::ManagedOutputChannel;
    JZProcess::ProcessState state;
    int exitCode;
    JZProcess::ExitStatus exitStatus;
    bool m_terminateRequested;
    // start() 失败原因（见 JZProcess::lastStartError）
    string startError;

    pid_t pid;
    int stdinWrite;
    int stdoutRead;
    int stderrRead;
    JZProcess *stdoutTarget = nullptr;   // 见 setStandardOutputProcess()

    JZByteArray stdoutBuffer;
    JZByteArray stderrBuffer;
    // 定时器线程与 waitForFinished() 的调用线程都会读管道 / 改缓冲区
    mutable JZMutex mutex;

private:
    // 辅助函数：尝试收尸并更新进程状态
    bool tryWaitForExit();
};

JZProcessPrivate::JZProcessPrivate(JZProcess *q)
    : q_ptr(q),
      channelMode(JZProcess::SeparateChannels),
      state(JZProcess::NotRunning),
      exitCode(0),
      exitStatus(JZProcess::NormalExit),
      m_terminateRequested(false),
      pid(-1),
      stdinWrite(-1),
      stdoutRead(-1),
      stderrRead(-1)
{
}

JZProcessPrivate::~JZProcessPrivate()
{
    // 修复1：析构时强制终止并回收运行中的进程，防止僵尸进程
    if (state == JZProcess::Running && pid > 0)
    {
        killProcess();
        waitForFinished(-1); // 无限等待直到进程结束
    }
    closeHandles();
}

void JZProcessPrivate::closeHandles()
{
    if (stdinWrite >= 0)
    {
        ::close(stdinWrite);
        stdinWrite = -1;
    }
    if (stdoutRead >= 0)
    {
        ::close(stdoutRead);
        stdoutRead = -1;
    }
    if (stderrRead >= 0)
    {
        ::close(stderrRead);
        stderrRead = -1;
    }
}

bool JZProcessPrivate::createProcess()
{
    const int execTimeoutMs = 1000;
    struct pollfd pfd;
    int pollRet;
    int elapsed = 0;
    const int pollStepMs = 10;

    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    bool stderrCreated = false;
    int childErr = 0;
    ssize_t n = 0;

    const int pipeFlags = O_NONBLOCK | O_CLOEXEC;
    if (::pipe2(stdinPipe, pipeFlags) == -1)
        goto cleanup_error;
    if (::pipe2(stdoutPipe, pipeFlags) == -1)
        goto cleanup_error;
    if (::pipe2(errorPipe, pipeFlags) == -1)
        goto cleanup_error;

    // 上游进程: 把 stdout 直接接到下游的 stdin 管道**写端**(见 setStandardOutputProcess)
    int targetFd = -1;
    if (stdoutTarget != nullptr)
    {
        targetFd = stdoutTarget->d_ptr->stdinWrite;
        if (targetFd < 0)
        {
            errno = EINVAL;
            startError = "下游进程还没有启动（先 start 下游，再 start 上游）";
            goto cleanup_error;
        }
        // 管道是用 O_CLOEXEC 建的: 不清掉的话子进程 exec 时会关掉, 上游就写不进去了
        ::fcntl(targetFd, F_SETFD, 0);
    }

    // 继承模式: 不建 stderr 管道 —— 子进程直接用我们自己的 fd 1 / fd 2
    const bool inheritOut = (outputMode == JZProcess::InheritOutputChannel);
    if (channelMode == JZProcess::SeparateChannels && !inheritOut)
    {
        if (::pipe2(stderrPipe, pipeFlags) == -1)
            goto cleanup_error;
        stderrCreated = true;
    }

    pid = ::fork();
    if (pid == -1)
        goto cleanup_error;

    if (pid == 0)
    {
        // ============ 子进程 ============
        // 自己当进程组组长(pgid = 本进程 pid): 之后 terminate/kill 用 kill(-pid) 就能把
        // **整棵进程树**一起收掉。只杀直接子进程的话, shell 底下的服务会变孤儿继续占端口。
        ::setpgid(0, 0);   // 失败也无妨(极少见): 退化成只杀直接子进程

        ::close(errorPipe[0]);

        // 继承模式: 子进程直接用我们自己的 stdin(不 dup2), 脚本里的命令才能接着读
        if (inputMode == JZProcess::ManagedInputChannel && ::dup2(stdinPipe[0], STDIN_FILENO) == -1)
        {
            int err = errno;
            ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
            (void)unused;
            _exit(1);
        }

        // 接了上游: 直接用下游的 stdin 管道; 否则用自己的 stdout 管道。
        // 继承模式: 什么都不做 —— fd 1 / 2 本来就是从我们这儿继承的(bash 的做法)。
        if (!inheritOut && ::dup2(targetFd >= 0 ? targetFd : stdoutPipe[1], STDOUT_FILENO) == -1)
        {
            int err = errno;
            ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
            (void)unused;
            _exit(1);
        }

        if (inheritOut)
        {
            // fd 2 也已经继承好了, 不动它
        }
        else if (stderrCreated)
        {
            if (::dup2(stderrPipe[1], STDERR_FILENO) == -1)
            {
                int err = errno;
                ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
                (void)unused;
                _exit(1);
            }
        }
        else
        {
            if (::dup2(targetFd >= 0 ? targetFd : stdoutPipe[1], STDERR_FILENO) == -1)
            {
                int err = errno;
                ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
                (void)unused;
                _exit(1);
            }
        }

        ::close(stdinPipe[0]);
        ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        if (stderrCreated)
        {
            ::close(stderrPipe[0]);
            ::close(stderrPipe[1]);
        }

        if (!workingDirectory.empty())
        {
            if (::chdir(workingDirectory.c_str()) == -1)
            {
                int err = errno;
                ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
                (void)unused;
                _exit(1);
            }
        }

        vector<char *> argv;
        argv.push_back(const_cast<char *>(program.c_str()));
        for (const auto &arg : arguments)
        {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        ::execvp(program.c_str(), argv.data());

        int err = errno;
        ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
        (void)unused;
        ::close(errorPipe[1]);
        _exit(1);
    }

    // ============ 父进程 ============
    ::close(errorPipe[1]);
    errorPipe[1] = -1;
    ::close(stdinPipe[0]);
    stdinPipe[0] = -1;
    ::close(stdoutPipe[1]);
    stdoutPipe[1] = -1;
    if (stderrCreated)
    {
        ::close(stderrPipe[1]);
        stderrPipe[1] = -1;
    }

    // ==================== 修复后的exec等待逻辑 ====================
    pfd.fd = errorPipe[0];
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (elapsed < execTimeoutMs)
    {
        pollRet = ::poll(&pfd, 1, pollStepMs);
        if (pollRet > 0)
            break;
        else if (pollRet == 0)
            elapsed += pollStepMs;
        else
        {
            if (errno == EINTR)
                continue;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
            pid = -1;
            goto cleanup_error;
        }
    }

    if (elapsed >= execTimeoutMs)
    {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
        pid = -1;
        errno = ETIMEDOUT;
        goto cleanup_error;
    }

    n = ::read(errorPipe[0], &childErr, sizeof(childErr));
    ::close(errorPipe[0]);
    errorPipe[0] = -1;

    if (n > 0)
    {
        int status;
        ::waitpid(pid, &status, 0);
        pid = -1;
        errno = childErr;
        goto cleanup_error;
    }
    else if (n == -1)
    {
        int status;
        ::waitpid(pid, &status, 0);
        pid = -1;
        goto cleanup_error;
    }
    // n == 0 表示exec成功，继续执行
    // ==============================================================

    if (inputMode == JZProcess::InheritInputChannel)
    {
        ::close(stdinPipe[1]);   // 继承模式: 不用我们喂
        stdinPipe[1] = -1;
        stdinWrite = -1;
    }
    else
    {
        stdinWrite = stdinPipe[1];
        stdinPipe[1] = -1;
    }
    stdoutRead = stdoutPipe[0];
    stdoutPipe[0] = -1;

    if (stdoutTarget != nullptr)
    {
        // 我们的 stdout 已经接到下游, 自己不再有 stdout 管道
        if (stdoutRead >= 0)
        {
            ::close(stdoutRead);
            stdoutRead = -1;
        }
        // 下游的 stdin 写端必须关掉: 否则上游退出后下游等不到 EOF, 会一直读
        if (stdoutTarget->d_ptr->stdinWrite >= 0)
        {
            ::close(stdoutTarget->d_ptr->stdinWrite);
            stdoutTarget->d_ptr->stdinWrite = -1;
        }
    }

    if (stderrCreated)
    {
        stderrRead = stderrPipe[0];
        stderrPipe[0] = -1;
    }

    state = JZProcess::Running;
    return true;

cleanup_error:
    if (stdinPipe[0] >= 0) ::close(stdinPipe[0]);
    if (stdinPipe[1] >= 0) ::close(stdinPipe[1]);
    if (stdoutPipe[0] >= 0) ::close(stdoutPipe[0]);
    if (stdoutPipe[1] >= 0) ::close(stdoutPipe[1]);
    if (errorPipe[0] >= 0) ::close(errorPipe[0]);
    if (errorPipe[1] >= 0) ::close(errorPipe[1]);
    if (stderrPipe[0] >= 0) ::close(stderrPipe[0]);
    if (stderrPipe[1] >= 0) ::close(stderrPipe[1]);

    // 失败原因给调用方：以前只回 bool，上层只能猜"程序不存在"；"工作目录不存在"这种就误导。
    // ENOENT 既可能是 chdir(工作目录)失败、也可能是 exec(程序)找不到 ⇒ 用目录是否存在区分。
    if (!startError.empty())
        return false;   // 上面已经给出更具体的原因(例如"下游进程还没启动")
    if (!workingDirectory.empty() && !JZFileInfo(workingDirectory).isDir())
        startError = "工作目录不存在或不是目录: " + workingDirectory;
    else if (errno != 0)
        startError = "无法启动 " + program + ": " + strerror(errno);
    else
        startError = "无法启动: " + program;

    return false;
}

void JZProcessPrivate::processRead()
{
    char buffer[4096];
    bool gotOut = false;
    bool gotErr = false;
    {
        // 定时器线程与 waitForFinished() 的调用线程都会读, 必须串起来
        JZMutexLocker lock(&mutex);

        // ============ 读取标准输出 ============
        while (stdoutRead >= 0)
        {
            ssize_t n = ::read(stdoutRead, buffer, sizeof(buffer));
            if (n > 0)
            {
                stdoutBuffer.append(buffer, int(n));
                gotOut = true;
                continue;
            }
            if (n == 0)   // 对端关闭: 排空后收尾
            {
                ::close(stdoutRead);
                stdoutRead = -1;
                break;
            }
            if (errno == EINTR)
                continue;   // 被信号打断, 重试
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;      // 暂时没数据
            ::close(stdoutRead);   // 真错误
            stdoutRead = -1;
            break;
        }

        // ============ 读取标准错误 ============
        if (channelMode == JZProcess::SeparateChannels)
        {
            while (stderrRead >= 0)
            {
                ssize_t n = ::read(stderrRead, buffer, sizeof(buffer));
                if (n > 0)
                {
                    stderrBuffer.append(buffer, int(n));
                    gotErr = true;
                    continue;
                }
                if (n == 0)
                {
                    ::close(stderrRead);
                    stderrRead = -1;
                    break;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                ::close(stderrRead);
                stderrRead = -1;
                break;
            }
        }
    }

    // 解锁之后再发信号: 槽里会调 readAllStandardOutput()/bytesAvailable(), 持锁发会自锁
    if (gotOut)
        q_ptr->sigReadyReadStandardOutput.emit();
    if (gotErr)
        q_ptr->sigReadyReadStandardError.emit();
}

// 辅助函数：非阻塞尝试收尸并更新进程状态
bool JZProcessPrivate::tryWaitForExit()
{
    if (state != JZProcess::Running || pid <= 0)
        return false;

    int status;
    pid_t ret = ::waitpid(pid, &status, WNOHANG);
    
    if (ret == pid)
    {
        // 进程确实已退出
        if (WIFEXITED(status))
        {
            exitCode = WEXITSTATUS(status);
            exitStatus = JZProcess::NormalExit;
        }
        else if (WIFSIGNALED(status))
        {
            exitCode = WTERMSIG(status);
            exitStatus = JZProcess::CrashExit;
        }

        processRead();

        state = JZProcess::NotRunning;
        closeHandles();
        q_ptr->finished(exitCode, exitStatus);
        return true;
    }
    else if (ret == -1)
    {
        state = JZProcess::NotRunning;
        closeHandles();
        q_ptr->finished(255, JZProcess::CrashExit);
        return true;
    }
    
    return false;
}

// 重点优化：onReadTimer 增加完整的进程退出检测逻辑
void JZProcessPrivate::onReadTimer()
{
    if(tryWaitForExit())
        return;

    processRead();
}

bool JZProcessPrivate::waitForFinished(int msecs)
{
    if (state == JZProcess::NotRunning && pid <= 0)
        return true;

    // msecs < 0 = 无限等(析构里就是这么调的)
    const bool forever = (msecs < 0);
    JZElapsedTimer t;
    t.start();

    while (forever || t.elapsed() < msecs)
    {
        // 等待期间持续收数据: 管道写满后子进程会阻塞, 不读就永远等不到退出
        processRead();
        if (tryWaitForExit())
            return true;

        JZThread::msleep(5);
    }
    return false;
}

int64_t JZProcessPrivate::write(const JZByteArray &data)
{
    if (state != JZProcess::Running || stdinWrite < 0)
        return -1;

    int64_t totalWritten = 0;
    const char *ptr = data.data();
    size_t remaining = data.size();

    while (remaining > 0)
    {
        ssize_t bytesWritten = ::write(stdinWrite, ptr, remaining);
        if (bytesWritten > 0)
        {
            totalWritten += bytesWritten;
            ptr += bytesWritten;
            remaining -= bytesWritten;
        }
        else if (bytesWritten == 0)
        {
            break; // 写入0字节，通常不会发生
        }
        else // bytesWritten == -1
        {
            if (errno == EINTR)
                continue; // 被信号中断，重试
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // 管道已满，返回已写入字节数（调用者需稍后重试）
            
            // 其他错误（如管道已关闭）
            return -1;
        }
    }

    return totalWritten;
}

void JZProcessPrivate::terminate()
{
    if (state != JZProcess::Running || pid <= 0)
        return;

    killTree(SIGTERM);
    closeHandles();
}

// 把**整棵进程树**干掉: 子进程启动时 setpgid(0,0) 成了进程组组长, 所以 kill(-pid) 会
// 命中整组(子进程的子孙默认继承这个组)。只杀直接子进程的话, shell 底下的服务会变孤儿
// 继续占端口 —— 这是"起了服务收不干净"的根因。
// 组不存在(kill(-pid) 失败)时退回杀它自己; 两个都失败说明进程已经没了, 顺手收尸。
void JZProcessPrivate::killTree(int sig)
{
    if (::kill(-pid, sig) == 0)
        return;
    if (::kill(pid, sig) == -1)
        tryWaitForExit();
}

void JZProcessPrivate::killProcess()
{
    if (state != JZProcess::Running || pid <= 0)
        return;

    killTree(SIGKILL);
}

void JZProcessPrivate::closeWriteChannel()
{
    if (stdinWrite >= 0)
    {
        ::close(stdinWrite);
        stdinWrite = -1;
    }
}

#else

static bool startDetachedProcess(const string &program, const vector<string> &arguments, const string &workingDirectory)
{
    return false;
}

class JZProcessPrivate
{
public:
    JZProcessPrivate(JZProcess* q)
    {

    }

    ~JZProcessPrivate()
    {

    }

    bool createProcess()
    {
        return false;
    }

    void closeHandles()
    {

    }

    void onReadTimer()
    {

    }

    bool waitForFinished(int msecs)
    {
        return false;
    }

    int64_t write(const JZByteArray &data)
    {
        return 0;
    }
    
    void terminate()
    {
    }

    void killProcess()
    {
    }
    void closeWriteChannel()
    {
    }

    void normal(JZByteArray &buffer)
    {
    }

    void processRead()
    {

    }

    bool tryWaitForExit()
    {
        return false;
    }

    JZProcess *q_ptr;
    string program;
    vector<string> arguments;
    string workingDirectory;
    JZProcess::ProcessChannelMode channelMode;
    JZProcess::ProcessState state;
    int exitCode;
    JZProcess::ExitStatus exitStatus;
    // 该平台不支持启动进程：留个字段让公共代码统一读（见 JZProcess::lastStartError）
    string startError;

    JZByteArray stdoutBuffer;
    JZByteArray stderrBuffer;
};

#endif

// ==================== JZProcess 公共部分 ====================

JZ_OBJECT_IMPL(JZProcess, JZObject)

JZProcess::JZProcess(JZObject *parent)
    : JZObject(parent),
      sigStarted(this),
      sigFinished(this),
      sigErrorOccurred(this),
      sigReadyReadStandardOutput(this),
      sigReadyReadStandardError(this),
      d_ptr(new JZProcessPrivate(this)),
      m_timerId(INVALID_TIMER_ID)
{
}

JZProcess::~JZProcess()
{
    killTimer(m_timerId);
    delete d_ptr;
}

bool JZProcess::startDetached(const string &program, const vector<string> &arguments, const string &workingDirectory)
{
    return startDetachedProcess(program, arguments, workingDirectory);
}

std::string JZProcess::lastStartError() const
{
    return m_startError;
}

bool JZProcess::start(const string &program, const vector<string> &arguments)
{
    if (d_ptr->state != NotRunning)
    {
        jzError() << "process is running.";
        return false;
    }

    d_ptr->program = program;
    d_ptr->arguments = arguments;
    d_ptr->stdoutTarget = m_stdoutTarget;   // 见 setStandardOutputProcess()
    d_ptr->inputMode = m_inputMode;         // 见 setInputChannelMode()
    d_ptr->outputMode = m_outputMode;       // 见 setOutputChannelMode()
    d_ptr->state = Starting;
    d_ptr->exitCode = 0;
    d_ptr->exitStatus = NormalExit;
    d_ptr->m_terminateRequested = false;
    d_ptr->stdoutBuffer.clear();
    d_ptr->stderrBuffer.clear();
    m_startError.clear();

    // 启动前先判工作目录：CreateProcess 对不存在的目录只回 ERROR_DIRECTORY(267)，而调用方常把它
    // 读成"程序不存在"（实测 agent_tool 的 run_js：还没编译、build/ 尚未生成就先跑）。这里提前判掉，
    // 给一条能指导下一步的准确原因；也顺带覆盖"工作目录指向一个文件"这种非法情况。
    if (!d_ptr->workingDirectory.empty() && !JZFileInfo(d_ptr->workingDirectory).isDir())
    {
        m_startError = "工作目录不存在或不是目录: " + d_ptr->workingDirectory
                       + "（先创建/编译出该目录，再运行）";
        jzError() << m_startError.c_str();
        d_ptr->state = NotRunning;
        sigErrorOccurred.emit(FailedToStart);
        return false;
    }

    if (!d_ptr->createProcess())
    {
        m_startError = d_ptr->startError.empty()
                           ? ("无法启动 " + program + "（程序不存在 / 无执行权限 / 被占用）")
                           : d_ptr->startError;
        d_ptr->state = NotRunning;
        sigErrorOccurred.emit(FailedToStart);
        return false;
    }

    d_ptr->state = Running;
    m_timerId = startTimer(10);

    sigStarted.emit();
    return true;
}

void JZProcess::timerEvent(JZTimerEvent *event)
{
    if (event->id() == m_timerId)
    {
        d_ptr->onReadTimer();
    }
}

void JZProcess::finished(int exitCode, JZProcess::ExitStatus exitStatus)
{
    killTimer(m_timerId);
    m_timerId = INVALID_TIMER_ID;

    d_ptr->state = NotRunning;
    d_ptr->closeHandles();
    // Windows 平台的 tryWaitForExit 只把退出码传给了 sigFinished 信号，
    // 没回写 d_ptr->exitCode，导致 waitForFinished() 后 exitCode() 恒为 0。
    d_ptr->exitCode = exitCode;
    d_ptr->exitStatus = exitStatus;

    sigFinished.emit(exitCode, exitStatus);
}

bool JZProcess::waitForFinished(int msecs)
{
    return d_ptr->waitForFinished(msecs);
}

JZByteArray JZProcess::readAllStandardOutput()
{
    JZMutexLocker lock(&d_ptr->mutex);
    JZByteArray result = d_ptr->stdoutBuffer;
    d_ptr->stdoutBuffer.clear();
    return result;
}

JZByteArray JZProcess::readAllStandardError()
{
    JZMutexLocker lock(&d_ptr->mutex);
    JZByteArray result = d_ptr->stderrBuffer;
    d_ptr->stderrBuffer.clear();
    return result;
}

// ---- 增量读(QProcess 那套) ----
// 当前读通道对应的缓冲区。read()/readLine()/bytesAvailable()/canReadLine() 共用。
static JZByteArray &channelBuffer(JZProcessPrivate *d, JZProcess::ProcessChannel ch)
{
    return (ch == JZProcess::StandardOutput) ? d->stdoutBuffer : d->stderrBuffer;
}

int64_t JZProcess::bytesAvailable() const
{
    JZMutexLocker lock(&d_ptr->mutex);
    return (int64_t)channelBuffer(d_ptr, m_readChannel).size();
}

bool JZProcess::canReadLine() const
{
    JZMutexLocker lock(&d_ptr->mutex);
    const JZByteArray &buf = channelBuffer(d_ptr, m_readChannel);
    for (int i = 0; i < buf.size(); i++)
    {
        if (buf[i] == '\n')
            return true;
    }
    return false;
}

JZByteArray JZProcess::read(int64_t maxSize)
{
    JZByteArray out;
    JZMutexLocker lock(&d_ptr->mutex);
    JZByteArray &buf = channelBuffer(d_ptr, m_readChannel);
    int n = buf.size();
    if (maxSize > 0 && maxSize < (int64_t)n)
        n = (int)maxSize;
    if (n <= 0)
        return out;
    out = buf.left(n);
    buf.remove(0, n);
    return out;
}

JZByteArray JZProcess::readLine(int64_t maxSize)
{
    JZByteArray out;
    JZMutexLocker lock(&d_ptr->mutex);
    JZByteArray &buf = channelBuffer(d_ptr, m_readChannel);
    int n = buf.size();
    if (maxSize > 0 && maxSize < (int64_t)n)
        n = (int)maxSize;
    if (n <= 0)
        return out;
    int end = n;
    for (int i = 0; i < n; i++)
    {
        if (buf[i] == '\n')
        {
            end = i + 1;
            break;
        }
    }
    out = buf.left(end);
    buf.remove(0, end);
    return out;
}

bool JZProcess::waitForReadyRead(int msecs)
{
    const bool forever = (msecs < 0);
    JZElapsedTimer t;
    t.start();
    while (forever || t.elapsed() < msecs)
    {
        d_ptr->processRead();
        if (bytesAvailable() > 0)
            return true;
        if (d_ptr->state != Running)
            return false;      // 进程已经结束, 不会再有数据了
        JZThread::msleep(5);
    }
    return bytesAvailable() > 0;
}

bool JZProcess::waitForStarted(int msecs)
{
    (void)msecs;   // start() 本来就是同步的(返回时进程已创建)
    return d_ptr->state == Running;
}

void JZProcess::setReadChannel(ProcessChannel channel)
{
    m_readChannel = channel;
}

JZProcess::ProcessChannel JZProcess::readChannel() const
{
    return m_readChannel;
}

int64_t JZProcess::write(const JZByteArray &data)
{
    return d_ptr->write(data);
}

void JZProcess::terminate()
{
    d_ptr->terminate();
}

void JZProcess::killProcess()
{
    d_ptr->killProcess();
}

void JZProcess::setWorkingDirectory(const string &dir)
{
    d_ptr->workingDirectory = dir;
}

string JZProcess::workingDirectory() const
{
    return d_ptr->workingDirectory;
}

void JZProcess::setProcessChannelMode(ProcessChannelMode mode)
{
    d_ptr->channelMode = mode;
}

void JZProcess::setInputChannelMode(InputChannelMode mode)
{
    m_inputMode = mode;
}

void JZProcess::setOutputChannelMode(OutputChannelMode mode)
{
    m_outputMode = mode;
}

JZProcess::OutputChannelMode JZProcess::outputChannelMode() const
{
    return m_outputMode;
}

JZProcess::InputChannelMode JZProcess::inputChannelMode() const
{
    return m_inputMode;
}

void JZProcess::setStandardOutputProcess(JZProcess *destination)
{
    m_stdoutTarget = destination;
}

JZProcess *JZProcess::standardOutputProcess() const
{
    return m_stdoutTarget;
}

JZProcess::ProcessChannelMode JZProcess::processChannelMode() const
{
    return d_ptr->channelMode;
}

JZProcess::ProcessState JZProcess::state() const
{
    return d_ptr->state;
}

int JZProcess::exitCode() const
{
    return d_ptr->exitCode;
}

int JZProcess::processId() const
{
    return d_ptr ? (int)d_ptr->pid : -1;
}

JZProcess::ExitStatus JZProcess::exitStatus() const
{
    return d_ptr->exitStatus;
}

string JZProcess::program() const
{
    return d_ptr->program;
}

vector<string> JZProcess::arguments() const
{
    return d_ptr->arguments;
}

void JZProcess::closeWriteChannel()
{
    d_ptr->closeWriteChannel();
}