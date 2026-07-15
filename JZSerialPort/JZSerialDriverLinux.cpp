#include "JZSerialDriverLinux.h"
#include "JZSerialPort.h"
#include "JZStd.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <termios.h>
#include <sys/ioctl.h>
#include <dirent.h>

#ifndef CMSPAR
#define CMSPAR 010000000000   // 标记/空格校验（Linux glibc 常定义，缺失时兜底）
#endif

// termios2 在 glibc 的 <termios.h> 中未导出，这里给出与内核一致的布局，
// 以避免引入 <asm/termios.h>（会与 glibc 的 struct termios 冲突）。
struct jz_termios2
{
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

#define JZ_TCGETS2 _IOR('T', 0x2A, struct jz_termios2)
#define JZ_TCSETS2 _IOWR('T', 0x2B, struct jz_termios2)
#define JZ_BOTHER  0010000

namespace
{
inline tcflag_t toLinuxDataBits(JZSerialPort::DataBits db)
{
    switch (db)
    {
    case JZSerialPort::Data5: return CS5;
    case JZSerialPort::Data6: return CS6;
    case JZSerialPort::Data7: return CS7;
    case JZSerialPort::Data8: return CS8;
    }
    return CS8;
}

// 通过 termios2 + BOTHER 支持任意波特率（包含标准与自定义速率）
bool setBaudRateFd(int fd, int baud)
{
    struct jz_termios2 tio;
    if (ioctl(fd, JZ_TCGETS2, &tio) < 0)
        return false;
    tio.c_cflag &= ~CBAUD;
    tio.c_cflag |= JZ_BOTHER;
    tio.c_ispeed = (speed_t)baud;
    tio.c_ospeed = (speed_t)baud;
    return ioctl(fd, JZ_TCSETS2, &tio) == 0;
}

// 将波特率映射到 glibc 已知的标准 Bxxx（作为 tcsetattr 的占位速率，
// 真正的任意速率由 setBaudRateFd 经 termios2 设置）。
speed_t mapStandardSpeed(int baud)
{
    switch (baud)
    {
    case 0: return B0;
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 500000: return B500000;
    case 576000: return B576000;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1152000: return B1152000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 2500000: return B2500000;
    case 3000000: return B3000000;
    case 3500000: return B3500000;
    case 4000000: return B4000000;
    default: return B38400;
    }
}
} // namespace

// ==================== 构造 / 析构 ====================
JZSerialDriverLinux::JZSerialDriverLinux() = default;

JZSerialDriverLinux::~JZSerialDriverLinux()
{
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto &kv : m_ports)
            names.push_back(kv.first);
    }
    for (auto &n : names)
    {
        JZSerialPort tmp(n);
        close(&tmp);
    }
}

std::string JZSerialDriverLinux::devicePath(const std::string &portName)
{
    // 形如 ttyUSB0 / /dev/ttyUSB0。相对名自动补 /dev/ 前缀。
    if (portName.compare(0, 5, "/dev/") == 0)
        return portName;
    return "/dev/" + portName;
}

JZSerialDriverLinux::PortState *JZSerialDriverLinux::stateOf(const std::string &portName)
{
    auto it = m_ports.find(portName);
    return (it == m_ports.end()) ? nullptr : &it->second;
}

// ==================== 接收线程 ====================
void JZSerialDriverLinux::readThread(JZSerialDriverLinux *self, JZSerialPort *port, PortState *st)
{
    const int bufSize = 256;
    std::vector<char> buf(bufSize);
    while (!st->stop.load(std::memory_order_relaxed))
    {
        ssize_t n = ::read(st->fd, buf.data(), bufSize);
        if (n > 0)
        {
            JZByteArray data(buf.data(), (int)n);
            port->onDataReceived(data);
        }
        else if (n < 0)
        {
            if (st->stop.load(std::memory_order_relaxed))
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 非阻塞无数据，主动让出时间片
                usleep(20000);
                continue;
            }
            // 其它错误（如设备移除）：稍作等待避免忙等
            usleep(20000);
        }
        // n == 0：EOF（如管道关闭），等待退出标志
    }
}

// ==================== 枚举可用串口 ====================
std::vector<JZSerialPortInfo> JZSerialDriverLinux::availablePorts()
{
    std::vector<JZSerialPortInfo> list;
    static const char *prefixes[] = {
        "ttyS", "ttyUSB", "ttyACM", "ttyAMA", "ttyO", "rfcomm", "ttyPS"
    };

    DIR *dir = opendir("/dev");
    if (!dir)
        return list;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr)
    {
        std::string name = ent->d_name;
        for (const char *p : prefixes)
        {
            if (name.compare(0, strlen(p), p) == 0)
            {
                JZSerialPortInfo info;
                info.id = "/dev/" + name;
                info.portName = "/dev/" + name;
                info.description = "Serial Port";
                list.push_back(info);
                break;
            }
        }
    }
    closedir(dir);
    return list;
}

// ==================== 打开 / 关闭 ====================
bool JZSerialDriverLinux::open(JZSerialPort *port)
{
    std::string path = devicePath(port->portName());

    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        jzWarning() << "JZSerialDriverLinux: open" << path << "failed, errno=" << errno;
        return false;
    }

    struct termios t;
    if (tcgetattr(fd, &t) < 0)
    {
        ::close(fd);
        return false;
    }

    // 原始模式：禁用回显/规范输入/信号处理
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~CSIZE;
    t.c_cflag |= toLinuxDataBits(port->dataBits());

    // 占位标准速率，避免 tcsetattr 应用 B0（挂断）；实际速率随后由 termios2 设置
    speed_t placeholder = mapStandardSpeed(port->baudRate());
    cfsetospeed(&t, placeholder);
    cfsetispeed(&t, placeholder);

    // 校验位
    t.c_cflag &= ~(PARENB | PARODD | CMSPAR);
    switch (port->parity())
    {
    case JZSerialPort::EvenParity: t.c_cflag |= PARENB; break;
    case JZSerialPort::OddParity:  t.c_cflag |= PARENB | PARODD; break;
    case JZSerialPort::SpaceParity: t.c_cflag |= PARENB | CMSPAR; break;
    case JZSerialPort::MarkParity:  t.c_cflag |= PARENB | CMSPAR | PARODD; break;
    case JZSerialPort::NoParity:
    default: break;
    }

    // 停止位
    if (port->stopBits() == JZSerialPort::TwoStop ||
        port->stopBits() == JZSerialPort::OneAndHalfStop)
        t.c_cflag |= CSTOPB;
    else
        t.c_cflag &= ~CSTOPB;

    // 流控
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cflag &= ~CRTSCTS;
    switch (port->flowControl())
    {
    case JZSerialPort::HardwareControl: t.c_cflag |= CRTSCTS; break;
    case JZSerialPort::SoftwareControl: t.c_iflag |= IXON | IXOFF; break;
    case JZSerialPort::NoFlowControl:
    default: break;
    }

    // VMIN=0, VTIME=2 => 读到任意数据即返回，或 0.2s 超时返回（配合 O_NONBLOCK 不会永久阻塞）
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 2;

    if (tcsetattr(fd, TCSANOW, &t) < 0)
    {
        ::close(fd);
        return false;
    }

    // 设置波特率（支持任意速率）
    if (!setBaudRateFd(fd, port->baudRate()))
    {
        ::close(fd);
        return false;
    }

    // 清空残留数据
    tcflush(fd, TCIOFLUSH);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        PortState &st = m_ports[port->portName()];
        st.fd = fd;
        st.running = true;
        st.stop.store(false, std::memory_order_relaxed);
        st.thread = std::thread(readThread, this, port, &st);
    }
    return true;
}

void JZSerialDriverLinux::close(JZSerialPort *port)
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
    if (st->fd >= 0)
    {
        // 关闭 fd 会使接收线程的 read() 立即返回 EBADF 并退出
        ::close(st->fd);
        st->fd = -1;
    }
    if (st->thread.joinable())
        st->thread.join();

    std::lock_guard<std::mutex> lk(m_mutex);
    m_ports.erase(name);
}

bool JZSerialDriverLinux::isOpen(JZSerialPort *port)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    return st && st->running && st->fd >= 0;
}

// ==================== 收发 ====================
int JZSerialDriverLinux::read(JZSerialPort *port, char *buf, int maxSize)
{
    if (maxSize <= 0)
        return -1;
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return -1;

    ssize_t n = ::read(st->fd, buf, (size_t)maxSize);
    if (n < 0)
        return -1; // 0: 暂无数据, -1: 错误
    return (int)n;
}

int JZSerialDriverLinux::write(JZSerialPort *port, const char *buf, int len)
{
    if (len <= 0)
        return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return 0;

    ssize_t n = ::write(st->fd, buf, (size_t)len);
    if (n < 0)
        return 0;
    return (int)n;
}

// ==================== 参数设置 ====================
bool JZSerialDriverLinux::setBaudRate(JZSerialPort *port, int32_t baudRate)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return false;
    return setBaudRateFd(st->fd, baudRate);
}

bool JZSerialDriverLinux::setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return false;

    struct termios t;
    if (tcgetattr(st->fd, &t) < 0)
        return false;
    t.c_cflag &= ~CSIZE;
    t.c_cflag |= toLinuxDataBits(dataBits);
    return tcsetattr(st->fd, TCSANOW, &t) == 0;
}

bool JZSerialDriverLinux::setParity(JZSerialPort *port, JZSerialPort::Parity parity)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return false;

    struct termios t;
    if (tcgetattr(st->fd, &t) < 0)
        return false;
    t.c_cflag &= ~(PARENB | PARODD | CMSPAR);
    switch (parity)
    {
    case JZSerialPort::EvenParity: t.c_cflag |= PARENB; break;
    case JZSerialPort::OddParity:  t.c_cflag |= PARENB | PARODD; break;
    case JZSerialPort::SpaceParity: t.c_cflag |= PARENB | CMSPAR; break;
    case JZSerialPort::MarkParity:  t.c_cflag |= PARENB | CMSPAR | PARODD; break;
    case JZSerialPort::NoParity:
    default: break;
    }
    return tcsetattr(st->fd, TCSANOW, &t) == 0;
}

bool JZSerialDriverLinux::setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return false;

    struct termios t;
    if (tcgetattr(st->fd, &t) < 0)
        return false;
    if (stopBits == JZSerialPort::TwoStop || stopBits == JZSerialPort::OneAndHalfStop)
        t.c_cflag |= CSTOPB;
    else
        t.c_cflag &= ~CSTOPB;
    return tcsetattr(st->fd, TCSANOW, &t) == 0;
}

bool JZSerialDriverLinux::setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    PortState *st = stateOf(port->portName());
    if (!st || st->fd < 0)
        return false;

    struct termios t;
    if (tcgetattr(st->fd, &t) < 0)
        return false;
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cflag &= ~CRTSCTS;
    switch (flowControl)
    {
    case JZSerialPort::HardwareControl: t.c_cflag |= CRTSCTS; break;
    case JZSerialPort::SoftwareControl: t.c_iflag |= IXON | IXOFF; break;
    case JZSerialPort::NoFlowControl:
    default: break;
    }
    return tcsetattr(st->fd, TCSANOW, &t) == 0;
}
