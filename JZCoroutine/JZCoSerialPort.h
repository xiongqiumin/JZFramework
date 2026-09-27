#ifndef JZCO_SERIAL_PORT_H_
#define JZCO_SERIAL_PORT_H_
#include "JZCo.h"
#include "JZSerialPort.h"
#include "JZResult.h"

// JZCoSerialPort 是 JZSerialPort 的协程封装。
// 底层 JZSerialPort 由驱动后台线程异步推送数据（onDataReceived -> sigReadyRead），
// 本类在收到数据时缓存并通过 JZCoCondition 唤醒等待中的协程，
// 使用户可以以「同步写法」在协程里读写串口，而不必自己处理信号/回调。
class JZFRAMEWORK_API JZCoSerialPort : public JZObject
{
public:
    JZCoSerialPort(JZObject *parent = nullptr);
    ~JZCoSerialPort();

    // 端口名 / 配置（直接转发到 JZSerialPort）
    void setPortName(const std::string &name);
    std::string portName() const;

    bool open();
    void close();
    bool isOpen() const;

    bool setBaudRate(int32_t baudRate);
    int32_t baudRate() const;
    bool setDataBits(JZSerialPort::DataBits dataBits);
    JZSerialPort::DataBits dataBits() const;
    bool setParity(JZSerialPort::Parity parity);
    JZSerialPort::Parity parity() const;
    bool setStopBits(JZSerialPort::StopBits stopBits);
    JZSerialPort::StopBits stopBits() const;
    bool setFlowControl(JZSerialPort::FlowControl flowControl);
    JZSerialPort::FlowControl flowControl() const;

    // 读：协程等待，超时返回 Error。ms < 0 表示一直等待。
    JZResult<JZByteArray> readAll(int ms = -1);
    JZResult<JZByteArray> read(int len, int ms = -1);

    // 按分隔符（默认 '\n'）读取一行（含分隔符）。
    JZResult<JZByteArray> readLine(int ms = -1, char delim = '\n');

    // 写
    void write(const JZByteArray &buffer);
    void write(const char *data, int len);

    // 硬件控制信号（DTR / RTS）
    bool setDtr(bool level);
    bool setRts(bool level);

    // ESP 常用快捷操作
    bool resetDevice();       // DTR=0, RTS=0 → 复位
    bool enterBootloader();   // DTR=0, RTS=1 → 进入下载模式

protected:
    enum WaitType
    {
        WaitNone,
        WaitRead,
    };

    void connectSignals();
    void yield(WaitType type, int timeout = -1);
    bool waitForReadyRead(int timeout);

    void onDataReceived();

    JZSerialPort *m_port;
    WaitType m_waitType;
    JZCoCondition m_coCtrl;
    JZByteArray m_recvBuffer;
};

#endif
