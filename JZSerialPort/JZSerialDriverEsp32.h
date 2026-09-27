#ifndef JZ_SERIAL_DRIVER_ESP32_H_
#define JZ_SERIAL_DRIVER_ESP32_H_
#include "JZSerialPortDriver.h"
#include <map>
#include <string>

// 基于 ESP-IDF UART 驱动的 JZSerialPortDriver 实现。
// 每个打开的端口对应一个 uart_port_t 和一个独立的接收任务，
// 收到数据后通过 port->onDataReceived() 推送给 JZSerialPort 并触发 sigReadyRead。
class JZFRAMEWORK_API JZSerialDriverEsp32 : public JZSerialPortDriver
{
public:
    JZSerialDriverEsp32();
    ~JZSerialDriverEsp32() override;

    // 在 open() 之前设置默认引脚；不设置则使用 -1（由芯片默认/不连接）。
    void setDefaultPins(int tx, int rx, int rts = -1, int cts = -1);

    bool open(JZSerialPort *port) override;
    void close(JZSerialPort *port) override;
    bool isOpen(JZSerialPort *port) override;

    std::vector<JZSerialPortInfo> availablePorts() override;

    int read(JZSerialPort *port, char *buf, int maxSize) override;
    int write(JZSerialPort *port, const char *buf, int len) override;

    bool setBaudRate(JZSerialPort *port, int32_t baudRate) override;
    bool setDataBits(JZSerialPort *port, JZSerialPort::DataBits dataBits) override;
    bool setParity(JZSerialPort *port, JZSerialPort::Parity parity) override;
    bool setStopBits(JZSerialPort *port, JZSerialPort::StopBits stopBits) override;
    bool setFlowControl(JZSerialPort *port, JZSerialPort::FlowControl flowControl) override;
    bool setDtr(JZSerialPort *port, bool level) override;
    bool setRts(JZSerialPort *port, bool level) override;

private:
    struct PortState
    {
        int  uartNum = -1;
        int  txPin = -1, rxPin = -1, rtsPin = -1, ctsPin = -1;
        bool running = false;
        void *taskHandle = nullptr;   // TaskHandle_t
    };

    static int  parseUartNum(const std::string &portName);
    static void readTask(void *arg);

    int  m_defTx = -1, m_defRx = -1, m_defRts = -1, m_defCts = -1;
    std::map<std::string, PortState> m_ports;
};

#endif // !JZ_SERIAL_DRIVER_ESP32_H_
