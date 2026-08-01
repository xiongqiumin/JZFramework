#include "JZSerialDriverWindows.h"
#include "JZSerialPort.h"
#include "JZStd.h"
#include "JZDebug.h"

#include <algorithm>

namespace
{
inline BYTE toWinParity(JZSerialPort::Parity p)
{
    switch (p)
    {
    case JZSerialPort::EvenParity: 
        return EVENPARITY;
    case JZSerialPort::OddParity:  
        return ODDPARITY;
    case JZSerialPort::SpaceParity: 
        return SPACEPARITY;
    case JZSerialPort::MarkParity:  
        return MARKPARITY;
    case JZSerialPort::NoParity:
        return NOPARITY;
    default:
        return NOPARITY;
    }
}

inline BYTE toWinStopBits(JZSerialPort::StopBits sb)
{
    switch (sb)
    {
    case JZSerialPort::OneStop:        return ONESTOPBIT;
    case JZSerialPort::OneAndHalfStop: return ONE5STOPBITS;
    case JZSerialPort::TwoStop:        return TWOSTOPBITS;
    }
    return ONESTOPBIT;
}

inline BYTE toWinDataBits(JZSerialPort::DataBits db)
{
    return (BYTE)db; // Data5..Data8 即 5..8
}
} // namespace

// ==================== 构造 / 析构 ====================
JZSerialDriverWindows::JZSerialDriverWindows() = default;

JZSerialDriverWindows::~JZSerialDriverWindows()
{
    // 关闭所有仍处于打开状态的端口（设置停止标志、关闭句柄并等待线程退出）
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto &kv : m_ports)
            names.push_back(kv.first);
    }
    for (auto &n : names)
    {
        // 构造临时端口对象以复用 close() 的按名查找逻辑
        JZSerialPort tmp(n);
        close(&tmp);
    }
}

std::string JZSerialDriverWindows::devicePath(const std::string &portName)
{
    // 形如 COM3 / COM10。大于 COM9 时需要使用 \\.\ 前缀。
    if (portName.compare(0, 4, "\\\\.\\") == 0)
        return portName;
    return "\\\\.\\" + portName;
}

JZSerialDriverWindows::PortState *JZSerialDriverWindows::stateOf(const std::string &portName)
{
    auto it = m_ports.find(portName);
    return (it == m_ports.end()) ? nullptr : &it->second;
}

// ==================== 接收线程 ====================
void JZSerialDriverWindows::readThread(JZSerialDriverWindows *self, JZSerialPort *port, PortState *st)
{
    const int bufSize = 256;
    std::vector<char> buf(bufSize);
    DWORD readLen = 0;
    while (!st->stop.load(std::memory_order_relaxed))
    {
        if (!ReadFile(st->handle, buf.data(), bufSize, &readLen, nullptr))
        {
            if (st->stop.load(std::memory_order_relaxed))
                break;
            // 设备移除或瞬时错误：稍作等待避免忙等
            Sleep(20);
            continue;
        }
        if (readLen > 0)
        {
            JZByteArray data(buf.data(), (int)readLen);
            port->onDataReceived(data);
        }
    }
}

// ==================== 枚举可用串口 ====================
std::vector<JZSerialPortInfo> JZSerialDriverWindows::availablePorts()
{
    std::vector<JZSerialPortInfo> list;

    // 从注册表 HKLM\HARDWARE\DEVICEMAP\SERIALCOMM 枚举当前存在的串口
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD idx = 0;
        char valueName[256];
        BYTE data[256];
        DWORD valueNameSize, dataSize, type;
        while (true)
        {
            valueNameSize = sizeof(valueName);
            dataSize = sizeof(data);
            LONG r = RegEnumValueA(hKey, idx++, valueName, &valueNameSize,
                                   nullptr, &type, data, &dataSize);
            if (r != ERROR_SUCCESS)
                break;
            if (type == REG_SZ && dataSize > 0)
            {
                std::string comName(reinterpret_cast<char *>(data));
                if (!comName.empty())
                {
                    JZSerialPortInfo info;
                    info.id = comName;
                    info.portName = comName;
                    info.description = "Serial Port";
                    list.push_back(info);
                }
            }
        }
        RegCloseKey(hKey);
    }
    return list;
}

// ==================== 打开 / 关闭 ====================
bool JZSerialDriverWindows::open(JZSerialPort *port)
{
    std::string name = port->portName();
    std::string path = devicePath(name);

    HANDLE h = CreateFileA(path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0,            // 独占
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        jzWarning() << "JZSerialDriverWindows: open" << path << "failed, error=" << GetLastError();
        return false;
    }

    // 配置超时：总超时 50ms，使接收线程以约 50ms 间隔轮询，且不阻塞同步 read()
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 50;
    SetCommTimeouts(h, &timeouts);

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb))
    {
        CloseHandle(h);
        return false;
    }
    dcb.BaudRate = (DWORD)port->baudRate();
    dcb.ByteSize = toWinDataBits(port->dataBits());
    dcb.Parity = toWinParity(port->parity());
    dcb.StopBits = toWinStopBits(port->stopBits());

    switch (port->flowControl())
    {
    case JZSerialPort::HardwareControl:
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        break;
    case JZSerialPort::SoftwareControl:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = TRUE;
        dcb.fInX = TRUE;
        break;
    case JZSerialPort::NoFlowControl:
    default:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        break;
    }

    if (!SetCommState(h, &dcb))
    {
        CloseHandle(h);
        return false;
    }

    // 清空可能存在的残留数据
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        PortState &st = m_ports[name];
        st.handle = h;
        st.running = true;
        st.stop.store(false, std::memory_order_relaxed);
        st.thread = std::thread(readThread, this, port, &st);
    }
    return true;
}

void JZSerialDriverWindows::close(JZSerialPort *port)
{
    std::string name = port->portName();
    PortState *st = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        st = stateOf(name);
        if (!st)
            return;
    }

    st->stop.store(true, std::memory_order_relaxed);
    if (st->handle != INVALID_HANDLE_VALUE)
    {
        CancelIo(st->handle);
        CloseHandle(st->handle);
        st->handle = INVALID_HANDLE_VALUE;
    }
    if (st->thread.joinable())
        st->thread.join();

    std::lock_guard<std::mutex> lk(m_mutex);
    m_ports.erase(name);
}

bool JZSerialDriverWindows::isOpen(JZSerialPort *port)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    return st && st->running && st->handle != INVALID_HANDLE_VALUE;
}

// ==================== 收发 ====================
int JZSerialDriverWindows::read(JZSerialPort *port, char *buf, int maxSize)
{
    if (maxSize <= 0)
        return -1;
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return -1;

    DWORD readLen = 0;
    if (!ReadFile(st->handle, buf, (DWORD)maxSize, &readLen, nullptr))
        return -1; // 0: 暂无数据, -1: 错误
    return (int)readLen;
}

int JZSerialDriverWindows::write(JZSerialPort *port, const char *buf, int len)
{
    if (len <= 0)
        return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return 0;

    DWORD written = 0;
    if (!WriteFile(st->handle, buf, (DWORD)len, &written, nullptr))
        return 0;
    return (int)written;
}

// ==================== 参数设置 ====================
bool JZSerialDriverWindows::setBaudRate(JZSerialPort *port, int32_t baudRate)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(st->handle, &dcb))
        return false;
    dcb.BaudRate = (DWORD)baudRate;
    return SetCommState(st->handle, &dcb) == TRUE;
}

bool JZSerialDriverWindows::setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(st->handle, &dcb))
        return false;
    dcb.ByteSize = toWinDataBits(dataBits);
    return SetCommState(st->handle, &dcb) == TRUE;
}

bool JZSerialDriverWindows::setParity(JZSerialPort *port, JZSerialPort::Parity parity)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(st->handle, &dcb))
        return false;
    dcb.Parity = toWinParity(parity);
    return SetCommState(st->handle, &dcb) == TRUE;
}

bool JZSerialDriverWindows::setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(st->handle, &dcb))
        return false;
    dcb.StopBits = toWinStopBits(stopBits);
    return SetCommState(st->handle, &dcb) == TRUE;
}

bool JZSerialDriverWindows::setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(st->handle, &dcb))
        return false;

    switch (flowControl)
    {
    case JZSerialPort::HardwareControl:
        dcb.fOutxCtsFlow = TRUE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        break;
    case JZSerialPort::SoftwareControl:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = TRUE;
        dcb.fInX = TRUE;
        break;
    case JZSerialPort::NoFlowControl:
    default:
        dcb.fOutxCtsFlow = FALSE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        break;
    }
    return SetCommState(st->handle, &dcb) == TRUE;
}

bool JZSerialDriverWindows::setDtr(JZSerialPort *port, bool level)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;
    return EscapeCommFunction(st->handle, level ? SETDTR : CLRDTR) != FALSE;
}

bool JZSerialDriverWindows::setRts(JZSerialPort *port, bool level)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->handle == INVALID_HANDLE_VALUE)
        return false;
    return EscapeCommFunction(st->handle, level ? SETRTS : CLRRTS) != FALSE;
}
