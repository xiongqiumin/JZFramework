#ifndef JZCO_PROCESS_H_
#define JZCO_PROCESS_H_

#include "JZCo.h"
#include "JZProcess.h"
#include "JZByteArray.h"
#include "JZResult.h"

struct JZCoProcessResult
{
    int exitCode = -1;
    JZProcess::ExitStatus exitStatus = JZProcess::NormalExit;
    JZByteArray stdOut;
    JZByteArray stdErr;
};

class JZCoProcess : public JZObject
{
public:
    JZCoProcess(JZObject *parent = nullptr);
    ~JZCoProcess();

    bool start(const std::string &program, const std::vector<std::string> &arguments = {});
    JZResult<JZCoProcessResult> waitForFinished(int ms = -1);

    JZByteArray readAllStandardOutput();
    JZByteArray readAllStandardError();

    int64_t write(const JZByteArray &data);

    void terminate();
    void killProcess();

    void setWorkingDirectory(const std::string &dir);
    std::string workingDirectory() const;

    void setProcessChannelMode(JZProcess::ProcessChannelMode mode);
    JZProcess::ProcessChannelMode processChannelMode() const;

    int exitCode() const;
    JZProcess::ExitStatus exitStatus() const;
    JZProcess::ProcessState state() const;
    std::string program() const;
    std::vector<std::string> arguments() const;
    void closeWriteChannel();

protected:
    enum WaitType
    {
        WaitNone,
        WaitFinish,
    };

    void connectSignals();
    void yield(WaitType type, int timeout = -1);

    void onFinished(int exitCode, JZProcess::ExitStatus exitStatus);
    void onError(JZProcess::ProcessError error);

    JZProcess *m_process;
    WaitType m_waitType;
    JZCoCondition m_coCtrl;
    int m_exitCode;
    JZProcess::ExitStatus m_exitStatus;
    std::string m_errorMsg;
};

#endif // !JZCO_PROCESS_H_
