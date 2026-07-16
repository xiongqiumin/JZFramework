#include "JZCoSerialPort.h"
#include "JZElapsedTimer.h"

#include <algorithm>

JZCoSerialPort::JZCoSerialPort(JZObject *parent)
    : JZObject(parent)
    , m_waitType(WaitNone)
{
    m_port = new JZSerialPort(this);
    connectSignals();
}

JZCoSerialPort::~JZCoSerialPort()
{
    close();
}

void JZCoSerialPort::connectSignals()
{
    connect(&m_port->sigReadyRead, this, &JZCoSerialPort::onDataReceived);
}

void JZCoSerialPort::yield(WaitType type, int timeout)
{
    m_waitType = type;
    m_coCtrl.wait(timeout);
    m_waitType = WaitNone;
}

void JZCoSerialPort::setPortName(const std::string &name)
{
    m_port->setPortName(name);
}

std::string JZCoSerialPort::portName() const
{
    return m_port->portName();
}

bool JZCoSerialPort::open()
{
    return m_port->open();
}

void JZCoSerialPort::close()
{
    m_port->close();
    m_recvBuffer.clear();
    // 若有关联协程正在等待数据，唤醒之（唤醒后其会读到 isOpen()==false 而退出）
    if (m_waitType != WaitNone)
        m_coCtrl.wakeAll();
    m_waitType = WaitNone;
}

bool JZCoSerialPort::isOpen() const
{
    return m_port->isOpen();
}

bool JZCoSerialPort::setBaudRate(int32_t baudRate)
{
    return m_port->setBaudRate(baudRate);
}

int32_t JZCoSerialPort::baudRate() const
{
    return m_port->baudRate();
}

bool JZCoSerialPort::setDataBits(JZSerialPort::DataBits dataBits)
{
    return m_port->setDataBits(dataBits);
}

JZSerialPort::DataBits JZCoSerialPort::dataBits() const
{
    return m_port->dataBits();
}

bool JZCoSerialPort::setParity(JZSerialPort::Parity parity)
{
    return m_port->setParity(parity);
}

JZSerialPort::Parity JZCoSerialPort::parity() const
{
    return m_port->parity();
}

bool JZCoSerialPort::setStopBits(JZSerialPort::StopBits stopBits)
{
    return m_port->setStopBits(stopBits);
}

JZSerialPort::StopBits JZCoSerialPort::stopBits() const
{
    return m_port->stopBits();
}

bool JZCoSerialPort::setFlowControl(JZSerialPort::FlowControl flowControl)
{
    return m_port->setFlowControl(flowControl);
}

JZSerialPort::FlowControl JZCoSerialPort::flowControl() const
{
    return m_port->flowControl();
}

//=======================================================================
// 信号处理：后台线程推送 -> 缓存 -> 唤醒等待协程
//=======================================================================
void JZCoSerialPort::onDataReceived()
{
    m_recvBuffer.append(m_port->readAll());
    if (m_waitType == WaitRead)
        m_coCtrl.wakeAll();
}

bool JZCoSerialPort::waitForReadyRead(int timeout)
{
    if (!isOpen())
        return false;

    yield(WaitRead, timeout);
    return !m_recvBuffer.isEmpty();
}

//=======================================================================
//  readAll -> JZResult
//=======================================================================
JZResult<JZByteArray> JZCoSerialPort::readAll(int ms)
{
    if (!m_recvBuffer.isEmpty())
    {
        JZByteArray buf = m_recvBuffer;
        m_recvBuffer.clear();
        return JZResult<JZByteArray>::Ok(buf);
    }

    if (!isOpen())
        return JZResult<JZByteArray>::Error("not open");

    if (!waitForReadyRead(ms))
        return JZResult<JZByteArray>::Error("timeout");

    JZByteArray buf = m_recvBuffer;
    m_recvBuffer.clear();
    return JZResult<JZByteArray>::Ok(buf);
}

//=======================================================================
//  read(len) -> JZResult：累积读到指定长度为止
//=======================================================================
JZResult<JZByteArray> JZCoSerialPort::read(int len, int ms)
{
    if (len <= 0)
        return JZResult<JZByteArray>::Error("invalid length");

    JZElapsedTimer t;
    if (ms >= 0)
        t.start();

    JZByteArray res;
    while (res.size() < len)
    {
        if (!m_recvBuffer.isEmpty())
        {
            int need = len - res.size();
            int take = std::min(need, (int)m_recvBuffer.size());
            res.append(m_recvBuffer.data(), take);
            m_recvBuffer = m_recvBuffer.mid(take);
            continue;
        }

        if (!isOpen())
            return JZResult<JZByteArray>::Error("not open");

        int waitMs = ms;
        if (ms >= 0)
        {
            waitMs = ms - t.elapsed();
            if (waitMs <= 0)
                return JZResult<JZByteArray>::Error("timeout");
        }

        if (!waitForReadyRead(waitMs))
            return JZResult<JZByteArray>::Error("timeout");
    }

    return JZResult<JZByteArray>::Ok(res);
}

//=======================================================================
//  readLine -> JZResult：读到分隔符为止（含分隔符）
//=======================================================================
JZResult<JZByteArray> JZCoSerialPort::readLine(int ms, char delim)
{
    JZElapsedTimer t;
    if (ms >= 0)
        t.start();

    while (true)
    {
        const char *d = m_recvBuffer.data();
        int n = (int)m_recvBuffer.size();
        int idx = -1;
        for (int i = 0; i < n; i++)
        {
            if (d[i] == delim)
            {
                idx = i;
                break;
            }
        }

        if (idx >= 0)
        {
            int len = idx + 1;
            JZByteArray line(m_recvBuffer.data(), len);
            m_recvBuffer = m_recvBuffer.mid(len);
            return JZResult<JZByteArray>::Ok(line);
        }

        if (!isOpen())
            return JZResult<JZByteArray>::Error("not open");

        int waitMs = ms;
        if (ms >= 0)
        {
            waitMs = ms - t.elapsed();
            if (waitMs <= 0)
                return JZResult<JZByteArray>::Error("timeout");
        }

        if (!waitForReadyRead(waitMs))
            return JZResult<JZByteArray>::Error("timeout");
    }
}

//=======================================================================
//  write
//=======================================================================
void JZCoSerialPort::write(const JZByteArray &buffer)
{
    m_port->write(buffer);
}

void JZCoSerialPort::write(const char *data, int len)
{
    m_port->write(data, len);
}
