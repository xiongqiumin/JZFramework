#include "JZCoProcess.h"

JZCoProcess::JZCoProcess(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
    , m_exitCode(-1)
    , m_exitStatus(JZProcess::NormalExit)
{
    m_process = new JZProcess(this);
    connectSignals();
}

JZCoProcess::~JZCoProcess()
{
}

void JZCoProcess::connectSignals()
{
    connect(&m_process->sigFinished, this, &JZCoProcess::onFinished);
    connect(&m_process->sigErrorOccurred, this, &JZCoProcess::onError);
}

void JZCoProcess::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

bool JZCoProcess::start(const std::string &program, const std::vector<std::string> &arguments)
{
    m_exitCode = -1;
    m_exitStatus = JZProcess::NormalExit;
    m_errorMsg.clear();
    return m_process->start(program, arguments);
}

void JZCoProcess::onFinished(int exitCode, JZProcess::ExitStatus exitStatus)
{
    m_exitCode = exitCode;
    m_exitStatus = exitStatus;
    if (m_waitType == WaitFinish)
        m_coCtrl.wakeAll();
}

void JZCoProcess::onError(JZProcess::ProcessError error)
{
    switch (error)
    {
    case JZProcess::FailedToStart: m_errorMsg = "failed to start"; break;
    case JZProcess::Crashed:       m_errorMsg = "crashed"; break;
    case JZProcess::Timedout:      m_errorMsg = "timedout"; break;
    case JZProcess::WriteError:    m_errorMsg = "write error"; break;
    case JZProcess::ReadError:     m_errorMsg = "read error"; break;
    case JZProcess::UnknownError:  m_errorMsg = "unknown error"; break;
    }
    if (m_waitType == WaitFinish)
        m_coCtrl.wakeAll();
}

JZResult<JZCoProcessResult> JZCoProcess::waitForFinished(int ms)
{
    // 进程已结束（快速退出或 start 失败）
    if (m_process->state() == JZProcess::NotRunning)
    {
        if (!m_errorMsg.empty())
            return JZResult<JZCoProcessResult>::Error(m_errorMsg);

        JZCoProcessResult result;
        result.exitCode = m_exitCode;
        result.exitStatus = m_exitStatus;
        result.stdOut = m_process->readAllStandardOutput();
        result.stdErr = m_process->readAllStandardError();
        return JZResult<JZCoProcessResult>::Ok(std::move(result));
    }

    // 等待 sigFinished / sigErrorOccurred 唤醒
    yield(WaitFinish, ms);

    if (!m_errorMsg.empty())
        return JZResult<JZCoProcessResult>::Error(m_errorMsg);

    if (m_process->state() != JZProcess::NotRunning)
        return JZResult<JZCoProcessResult>::Error("timeout");

    JZCoProcessResult result;
    result.exitCode = m_exitCode;
    result.exitStatus = m_exitStatus;
    result.stdOut = m_process->readAllStandardOutput();
    result.stdErr = m_process->readAllStandardError();
    return JZResult<JZCoProcessResult>::Ok(std::move(result));
}

JZByteArray JZCoProcess::readAllStandardOutput()
{
    return m_process->readAllStandardOutput();
}

JZByteArray JZCoProcess::readAllStandardError()
{
    return m_process->readAllStandardError();
}

int64_t JZCoProcess::write(const JZByteArray &data)
{
    return m_process->write(data);
}

void JZCoProcess::terminate()
{
    m_process->terminate();
}

void JZCoProcess::killProcess()
{
    m_process->killProcess();
}

void JZCoProcess::setWorkingDirectory(const std::string &dir)
{
    m_process->setWorkingDirectory(dir);
}

std::string JZCoProcess::workingDirectory() const
{
    return m_process->workingDirectory();
}

void JZCoProcess::setProcessChannelMode(JZProcess::ProcessChannelMode mode)
{
    m_process->setProcessChannelMode(mode);
}

JZProcess::ProcessChannelMode JZCoProcess::processChannelMode() const
{
    return m_process->processChannelMode();
}

int JZCoProcess::exitCode() const
{
    return m_process->exitCode();
}

JZProcess::ExitStatus JZCoProcess::exitStatus() const
{
    return m_process->exitStatus();
}

JZProcess::ProcessState JZCoProcess::state() const
{
    return m_process->state();
}

std::string JZCoProcess::program() const
{
    return m_process->program();
}

std::vector<std::string> JZCoProcess::arguments() const
{
    return m_process->arguments();
}

void JZCoProcess::closeWriteChannel()
{
    m_process->closeWriteChannel();
}
