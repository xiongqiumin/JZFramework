#ifndef JZ_SERIAL_PORT_DRIVER_H_
#define JZ_SERIAL_PORT_DRIVER_H_
#include <string>
#include <vector>
#include "JZSerialPort.h"
#include "JZSerialPortInfo.h"

// 串口驱动抽象基类（单例）。具体平台（如 ESP32 / AC79 / 主机）实现自己的子类，
// 并在程序初始化时通过 setInstance() 安装。JZSerialPort 的所有底层操作都委派给它。
class JZFRAMEWORK_API JZSerialPortDriver
{
public:
    JZSerialPortDriver() = default;
    virtual ~JZSerialPortDriver() = default;

    static JZSerialPortDriver *instance();
    static void setInstance(JZSerialPortDriver *inst);

    // 枚举当前系统可用串口。各平台驱动自行实现。
    virtual std::vector<JZSerialPortInfo> availablePorts() = 0;

    // 打开 / 关闭 / 查询
    virtual bool open(JZSerialPort *port) = 0;
    virtual void close(JZSerialPort *port) = 0;
    virtual bool isOpen(JZSerialPort *port) = 0;

    // 同步收发（辅助用）。数据主要经 onDataReceived 异步推送。
    virtual int read(JZSerialPort *port, char *buf, int maxSize) = 0;
    virtual int write(JZSerialPort *port, const char *buf, int len) = 0;

    // 参数设置
    virtual bool setBaudRate(JZSerialPort *port, int32_t baudRate) = 0;
    virtual bool setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits) = 0;
    virtual bool setParity(JZSerialPort *port, JZSerialPort::Parity parity) = 0;
    virtual bool setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits) = 0;
    virtual bool setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl) = 0;

    // 硬件控制信号（DTR / RTS）。用于触发目标芯片复位/烧录模式。
    virtual bool setDtr(JZSerialPort *port, bool level) = 0;
    virtual bool setRts(JZSerialPort *port, bool level) = 0;

protected:
    JZ_DISABLE_COPY(JZSerialPortDriver)

    static JZSerialPortDriver *s_instance;
};

#endif // !JZ_SERIAL_PORT_DRIVER_H_
