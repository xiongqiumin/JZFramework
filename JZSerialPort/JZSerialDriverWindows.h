#ifndef JZ_SERIAL_DRIVER_WINDOWS_H_
#define JZ_SERIAL_DRIVER_WINDOWS_H_

#include "JZSerialPortDriver.h"
#include <windows.h>
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

// 基于 Win32 API (CreateFile / COM 端口) 的 JZSerialPortDriver 实现。
// 每个打开的端口对应一个 HANDLE 和一个独立的接收线程，
// 收到数据后通过 port->onDataReceived() 推送给 JZSerialPort 并触发 sigReadyRead。
class JZSerialDriverWindows : public JZSerialPortDriver
{
public:
    JZSerialDriverWindows();
    ~JZSerialDriverWindows() override;

    std::vector<JZSerialPortInfo> availablePorts() override;

    bool open(JZSerialPort *port) override;
    void close(JZSerialPort *port) override;
    bool isOpen(JZSerialPort *port) override;

    int read(JZSerialPort *port, char *buf, int maxSize) override;
    int write(JZSerialPort *port, const char *buf, int len) override;

    bool setBaudRate(JZSerialPort *port, int32_t baudRate) override;
    bool setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits) override;
    bool setParity(JZSerialPort *port, JZSerialPort::Parity parity) override;
    bool setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits) override;
    bool setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl) override;

private:
    struct PortState
    {
        HANDLE       handle = INVALID_HANDLE_VALUE;
        bool         running = false;
        std::thread  thread;
        std::atomic<bool> stop{false};
    };

    static void readThread(JZSerialDriverWindows *self, JZSerialPort *port, PortState *st);
    static std::string devicePath(const std::string &portName);

    PortState *stateOf(const std::string &portName);

    std::map<std::string, PortState> m_ports;
    std::mutex m_mutex;
};

#endif // !JZ_SERIAL_DRIVER_WINDOWS_H_
