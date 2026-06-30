#include "JZProcess.h"
#include "JZDebug.h"
#include "JZElapsedTimer.h"
#include "JZFileInfo.h"
#include "JZDir.h"
#include "JZTx.h"

using namespace std;

// ==================== Windows 平台 ====================
#ifdef _WIN32

#include <Windows.h>

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
    JZProcess::ProcessState state;
    int exitCode;
    JZProcess::ExitStatus exitStatus;

    HANDLE hProcess;
    HANDLE hThread;
    HANDLE hStdinWrite;
    char stdOutTail; 
    HANDLE hStdoutRead;
    char stdErrTail;
    HANDLE hStderrRead;

    JZByteArray stdoutBuffer;
    JZByteArray stderrBuffer;
};

JZProcessPrivate::JZProcessPrivate(JZProcess *q)
    : q_ptr(q),
      channelMode(JZProcess::SeparateChannels),
      state(JZProcess::NotRunning),
      exitCode(0),
      exitStatus(JZProcess::NormalExit),
      hProcess(INVALID_HANDLE_VALUE),
      hThread(INVALID_HANDLE_VALUE),
      hStdinWrite(INVALID_HANDLE_VALUE),
      hStdoutRead(INVALID_HANDLE_VALUE),
      hStderrRead(INVALID_HANDLE_VALUE)
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
}

bool JZProcessPrivate::createProcess()
{
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE hStdinRead = nullptr, hStdoutWrite = nullptr, hStderrWrite = nullptr;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0) || !CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0))
    {
        return false;
    }

    if (channelMode == JZProcess::SeparateChannels)
    {
        if (!CreatePipe(&hStderrRead, &hStderrWrite, &sa, 0))
        {
            CloseHandle(hStdinRead);
            CloseHandle(hStdinWrite);
            CloseHandle(hStdoutRead);
            CloseHandle(hStdoutWrite);
            hStdinRead = hStdinWrite = hStdoutRead = hStdoutWrite = INVALID_HANDLE_VALUE;
            return false;
        }
    }

    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);
    if (channelMode == JZProcess::SeparateChannels)
    {
        SetHandleInformation(hStderrRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);
    si.hStdInput = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError = (channelMode == JZProcess::SeparateChannels) ? hStderrWrite : hStdoutWrite;
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

    string cmdLine = program;
    for (const auto &arg : arguments)
    {
        if (str_contains(arg, " "))
            cmdLine += " \"" + arg + "\"";
        else
            cmdLine += " " + arg;
    }

    BOOL success;
    if (!workingDirectory.empty())
    {
        string work_dir = workingDirectory;
        str_replace(work_dir, "/", "\\");
        success = CreateProcessA(nullptr, const_cast<char *>(cmdLine.c_str()), nullptr, nullptr, TRUE, 0, nullptr,
                                 work_dir.c_str(), &si, &pi);
    }
    else
    {
        success = CreateProcessA(nullptr, const_cast<char *>(cmdLine.c_str()), nullptr, nullptr, TRUE, 0, nullptr,
                                 nullptr, &si, &pi);
    }

    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);
    hStdinRead = hStdoutWrite = INVALID_HANDLE_VALUE;
    if (channelMode == JZProcess::SeparateChannels)
    {
        CloseHandle(hStderrWrite);
    }

    if (!success)
    {
        CloseHandle(hStdinWrite);
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

void JZProcessPrivate::processRead()
{
    char buffer[4096];
    DWORD bytesRead;

    if (ReadFile(hStdoutRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        if (stdOutTail != 0)
        {
            stdoutBuffer.push_back(stdOutTail);
            stdOutTail = 0;
        }
        if (buffer[bytesRead - 1] == '\r')
        {
            stdOutTail = '\r';
            bytesRead--;
        }
        stdoutBuffer.append(buffer, (int)bytesRead);
        normal(stdoutBuffer);

        q_ptr->sigReadyReadStandardOutput.emit();
    }

    if (channelMode == JZProcess::SeparateChannels)
    {
        if (ReadFile(hStderrRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            if (stdErrTail != 0)
            {
                stderrBuffer.push_back(stdErrTail);
                stdErrTail = 0;
            }
            if (buffer[bytesRead - 1] == '\r')
            {
                stdErrTail = '\r';
                bytesRead--;
            }
            stderrBuffer.append(buffer, (int)bytesRead);
            normal(stderrBuffer);

            q_ptr->sigReadyReadStandardError.emit();
        }
    }
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
        q_ptr->finished(exitCode, JZProcess::NormalExit);
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

    JZElapsedTimer t;
    t.start();
    
    while(t.elapsed() < msecs)
    {
        if(tryWaitForExit())
            return true;

        JZThread::msleep(20);
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
    if (state != JZProcess::Running || hProcess == nullptr)
    {
        return;
    }
    TerminateProcess(hProcess, 1);
    closeHandles();
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

    JZProcess *q_ptr;
    string program;
    vector<string> arguments;
    string workingDirectory;
    JZProcess::ProcessChannelMode channelMode;
    JZProcess::ProcessState state;
    int exitCode;
    JZProcess::ExitStatus exitStatus;

    pid_t pid;
    int stdinWrite;
    int stdoutRead;
    int stderrRead;

    JZByteArray stdoutBuffer;
    JZByteArray stderrBuffer;

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

    if (channelMode == JZProcess::SeparateChannels)
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
        ::close(errorPipe[0]);

        if (::dup2(stdinPipe[0], STDIN_FILENO) == -1)
        {
            int err = errno;
            ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
            (void)unused;
            _exit(1);
        }

        if (::dup2(stdoutPipe[1], STDOUT_FILENO) == -1)
        {
            int err = errno;
            ssize_t unused = ::write(errorPipe[1], &err, sizeof(err));
            (void)unused;
            _exit(1);
        }

        if (stderrCreated)
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
            if (::dup2(stdoutPipe[1], STDERR_FILENO) == -1)
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

    stdinWrite = stdinPipe[1];
    stdoutRead = stdoutPipe[0];
    stdinPipe[1] = -1;
    stdoutPipe[0] = -1;

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

    return false;
}

void JZProcessPrivate::processRead()
{
    char buffer[4096];
    ssize_t bytesRead;

    // ============ 读取标准输出 ============
    if (stdoutRead >= 0)
    {
        while (true)
        {
            bytesRead = ::read(stdoutRead, buffer, sizeof(buffer));
 
            if (bytesRead > 0)
            {
                stdoutBuffer.append(buffer, int(bytesRead));
                q_ptr->sigReadyReadStandardOutput.emit();
            }
            else if (bytesRead == 0)
            {
                // 管道对端关闭，但可能还有数据在缓冲区
                // 继续读取直到没有更多数据
                ::close(stdoutRead);
                stdoutRead = -1;
                break;
            }
            else // bytesRead == -1
            {
                if (errno == EINTR)
                    continue; // 被信号中断，重试
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // 暂时无数据，退出循环
                
                // 发生错误，关闭管道
                ::close(stdoutRead);
                stdoutRead = -1;
                break;
            }
        }
    }

    // ============ 读取标准错误 ============
    if (channelMode == JZProcess::SeparateChannels && stderrRead >= 0)
    {
        while (true)
        {
            bytesRead = ::read(stderrRead, buffer, sizeof(buffer));
            if (bytesRead > 0)
            {
                stderrBuffer.append(buffer, int(bytesRead));
                q_ptr->sigReadyReadStandardError.emit();
            }
            else if (bytesRead == 0)
            {
                ::close(stderrRead);
                stderrRead = -1;
                break;
            }
            else
            {
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

    JZElapsedTimer t;
    t.start();
    
    while(t.elapsed() < msecs)
    {
        if(tryWaitForExit())
            return true;

        JZThread::msleep(20);
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

    if (::kill(pid, SIGTERM) == -1)
    {
        // kill失败通常意味着进程已经退出，尝试收尸
        tryWaitForExit();
    }
    closeHandles();
}

void JZProcessPrivate::killProcess()
{
    if (state != JZProcess::Running || pid <= 0)
        return;

    if (::kill(pid, SIGKILL) == -1)
    {
        tryWaitForExit();
    }
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
      m_timerId(INVAILD_TIMER_ID)
{
}

JZProcess::~JZProcess()
{
    killTimer(m_timerId);
    delete d_ptr;
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
    d_ptr->state = Starting;
    d_ptr->exitCode = 0;
    d_ptr->exitStatus = NormalExit;
    d_ptr->stdoutBuffer.clear();
    d_ptr->stderrBuffer.clear();

    if (!d_ptr->createProcess())
    {
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
    m_timerId = INVAILD_TIMER_ID;

    d_ptr->state = NotRunning;
    d_ptr->closeHandles();

    sigFinished.emit(exitCode, exitStatus);
}

bool JZProcess::waitForFinished(int msecs)
{
    return d_ptr->waitForFinished(msecs);
}

JZByteArray JZProcess::readAllStandardOutput()
{
    JZByteArray result = d_ptr->stdoutBuffer;
    d_ptr->stdoutBuffer.clear();
    return result;
}

JZByteArray JZProcess::readAllStandardError()
{
    JZByteArray result = d_ptr->stderrBuffer;
    d_ptr->stderrBuffer.clear();
    return result;
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