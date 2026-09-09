#ifndef JZPROCESS_H
#define JZPROCESS_H

#include "JZObject.h"
#include "JZByteArray.h"
#include "JZThread.h"
#include <vector>
#include <string>

class JZProcessPrivate;

class JZProcess : public JZObject
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

    void setProcessChannelMode(ProcessChannelMode mode);
    ProcessChannelMode processChannelMode() const;

    ProcessState state() const;
    int exitCode() const;
    ExitStatus exitStatus() const;

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
};

#endif
