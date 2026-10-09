#ifndef JZPROCESS_H
#define JZPROCESS_H
#include "JZObject.h"
#include "JZByteArray.h"
#include "JZThread.h"
#include <vector>
#include <string>

class JZProcessPrivate;

class JZFRAMEWORK_API JZProcess : public JZObject
{
    JZ_OBJECT_DECLARE(JZProcess)
public:
    enum ProcessState {
        NotRunning,
        Starting,
        Running
    };

    enum ProcessChannelMode {
        SeparateChannels,
        MergedChannels,
        ForwardedChannels
    };

    enum ExitStatus {
        NormalExit,
        CrashExit
    };

    enum ProcessError {
        FailedToStart,
        Crashed,
        Timedout,
        WriteError,
        ReadError,
        UnknownError
    };

    // 启动一个分离进程: 不创建管道、不跟踪状态、不回收, 父进程退出后子进程独立存活。
    // 成功返回 true (仅表示创建成功, 不等待子进程结束)。
    static bool startDetached(const std::string &program,
                              const std::vector<std::string> &arguments = {},
                              const std::string &workingDirectory = {});

    explicit JZProcess(JZObject *parent = nullptr);
    ~JZProcess();

    bool start(const std::string &program, const std::vector<std::string> &arguments = {});
    bool waitForFinished(int msecs = 30000);

    JZByteArray readAllStandardOutput();
    JZByteArray readAllStandardError();

    int64_t write(const JZByteArray &data);

    void terminate();
    void killProcess();

    void setWorkingDirectory(const std::string &dir);
    std::string workingDirectory() const;

    // 上一次 start() 失败的原因（已按失败类型归类：工作目录不存在 / 找不到程序 / 拒绝访问 / 错误码 …）。
    // start() 成功时清空。⚠ 以前只回 bool，调用方只能猜，于是把"工作目录不存在"误报成"程序不存在"
    // （实测：run_js 在还没编译、build/ 尚未生成时启动，CreateProcess 回 ERROR_DIRECTORY 267）。
    std::string lastStartError() const;

    void setProcessChannelMode(ProcessChannelMode mode);
    ProcessChannelMode processChannelMode() const;

    ProcessState state() const;
    int exitCode() const;
    ExitStatus exitStatus() const;

    // 真实操作系统进程号(Linux = fork 出来的 pid; Windows = CreateProcess 的 dwProcessId)。
    // 未启动 / 已回收返回 -1。JZVirtualTerminal 用它把"运行记录"和真实进程对应起来。
    int processId() const;

    std::string program() const;
    std::vector<std::string> arguments() const;

    void closeWriteChannel();

jz_signals:
    JZSignal<> sigStarted;
    JZSignal<int, JZProcess::ExitStatus> sigFinished;
    JZSignal<ProcessError> sigErrorOccurred;
    JZSignal<> sigReadyReadStandardOutput;
    JZSignal<> sigReadyReadStandardError;

private:
    friend JZProcessPrivate;

    virtual void timerEvent(JZTimerEvent *event) override;
    void finished(int, JZProcess::ExitStatus);

    JZProcessPrivate *d_ptr;
    int m_timerId;
    std::string m_startError;   // 见 lastStartError()
};

#endif
