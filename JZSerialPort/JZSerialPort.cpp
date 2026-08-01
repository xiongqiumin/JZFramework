#include "JZSerialPort.h"
#include "JZSerialPortDriver.h"

JZSerialPort::JZSerialPort(JZObject *parent)
    : JZObject(parent), sigReadyRead(this)
{
}

JZSerialPort::JZSerialPort(const std::string &name, JZObject *parent)
    : JZObject(parent), sigReadyRead(this)
{
    m_portName = name;
}

JZSerialPort::~JZSerialPort()
{
    close();
}

void JZSerialPort::setPortName(const std::string &name)
{
    m_portName = name;
}

std::string JZSerialPort::portName() const
{
    return m_portName;
}

std::vector<JZSerialPortInfo> JZSerialPort::availablePorts()
{
    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
        return drv->availablePorts();
    return {};
}

JZSerialPortInfo JZSerialPort::portInfo() const
{
    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
    {
        for (auto &info : drv->availablePorts())
        {
            if (info.portName == m_portName)
                return info;
        }
    }

    // 驱动无法枚举时，仅返回已知端口名
    JZSerialPortInfo info;
    info.id = m_portName;
    info.portName = m_portName;
    return info;
}

bool JZSerialPort::open()
{
    JZSerialPortDriver *drv = JZSerialPortDriver::instance();
    if (!drv)
        return false;

    m_open = drv->open(this);
    return m_open;
}

void JZSerialPort::close()
{
    if (!m_open)
        return;

    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
        drv->close(this);

    m_open = false;
    m_readBuffer.clear();
}

bool JZSerialPort::isOpen() const
{
    return m_open;
}

JZByteArray JZSerialPort::read(int max_size)
{
    int n = (int)m_readBuffer.size();
    if (max_size < n)
        n = max_size;

    JZByteArray res(m_readBuffer.data(), n);
    m_readBuffer = m_readBuffer.mid(n);
    return res;
}

int JZSerialPort::read(char *data, int max_size)
{
    int n = (int)m_readBuffer.size();
    if (max_size < n)
        n = max_size;

    if (n > 0)
        memcpy(data, m_readBuffer.data(), n);
    m_readBuffer = m_readBuffer.mid(n);
    return n;
}

JZByteArray JZSerialPort::readAll()
{
    JZByteArray res = m_readBuffer;
    m_readBuffer.clear();
    return res;
}

void JZSerialPort::write(const JZByteArray &buffer)
{
    write(buffer.data(), buffer.size());
}

void JZSerialPort::write(const char *data, int len)
{
    if (!m_open)
        return;
    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
        drv->write(this, data, len);
}

bool JZSerialPort::setBaudRate(int32_t baudRate)
{
    m_baudRate = baudRate;
    if (m_open)
    {
        if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
            return drv->setBaudRate(this, baudRate);
        return false;
    }
    return true;
}

int32_t JZSerialPort::baudRate() const
{
    return m_baudRate;
}

bool JZSerialPort::setDataBits(DataBits dataBits)
{
    m_dataBits = dataBits;
    if (m_open)
    {
        if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
            return drv->setDataBits(this, dataBits);
        return false;
    }
    return true;
}

JZSerialPort::DataBits JZSerialPort::dataBits() const
{
    return m_dataBits;
}

bool JZSerialPort::setParity(Parity parity)
{
    m_parity = parity;
    if (m_open)
    {
        if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
            return drv->setParity(this, parity);
        return false;
    }
    return true;
}

JZSerialPort::Parity JZSerialPort::parity() const
{
    return m_parity;
}

bool JZSerialPort::setStopBits(StopBits stopBits)
{
    m_stopBits = stopBits;
    if (m_open)
    {
        if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
            return drv->setStopBits(this, stopBits);
        return false;
    }
    return true;
}

JZSerialPort::StopBits JZSerialPort::stopBits() const
{
    return m_stopBits;
}

bool JZSerialPort::setFlowControl(FlowControl flowControl)
{
    m_flowControl = flowControl;
    if (m_open)
    {
        if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
            return drv->setFlowControl(this, flowControl);
        return false;
    }
    return true;
}

JZSerialPort::FlowControl JZSerialPort::flowControl() const
{
    return m_flowControl;
}

bool JZSerialPort::setDtr(bool level)
{
    if (!m_open)
        return false;
    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
        return drv->setDtr(this, level);
    return false;
}

bool JZSerialPort::setRts(bool level)
{
    if (!m_open)
        return false;
    if (JZSerialPortDriver *drv = JZSerialPortDriver::instance())
        return drv->setRts(this, level);
    return false;
}

void JZSerialPort::onDataReceived(const JZByteArray &data)
{
    if (data.size() <= 0)
        return;

    m_readBuffer.append(data.data(), data.size());
    sigReadyRead.emit();
}
