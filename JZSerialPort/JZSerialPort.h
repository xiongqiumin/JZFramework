#ifndef JZSerialPort_H
#define JZSerialPort_H
#include "JZObject.h"
#include "JZByteArray.h"
#include "JZSerialPortInfo.h"

// JZSerialPort 是面向用户的串口类。它自身不做平台相关的收发，
// 所有底层操作都委派给单例 JZSerialPortDriver（例如 JZSerialDriverEsp32）。
// 接收到的数据由驱动通过 onDataReceived() 异步推送进来，并通过 sigReadyRead 通知。
class JZFRAMEWORK_API JZSerialPort : public JZObject
{
public:

    enum Direction  {
        Input = 1,
        Output = 2,
        AllDirections = Input | Output
    };

    enum BaudRate {
        Baud1200 = 1200,
        Baud2400 = 2400,
        Baud4800 = 4800,
        Baud9600 = 9600,
        Baud19200 = 19200,
        Baud38400 = 38400,
        Baud57600 = 57600,
        Baud115200 = 115200
    };

    enum DataBits {
        Data5 = 5,
        Data6 = 6,
        Data7 = 7,
        Data8 = 8
    };

    enum Parity {
        NoParity = 0,
        EvenParity = 2,
        OddParity = 3,
        SpaceParity = 4,
        MarkParity = 5
    };

    enum StopBits {
        OneStop = 1,
        OneAndHalfStop = 3,
        TwoStop = 2
    };

    enum FlowControl {
        NoFlowControl,
        HardwareControl,
        SoftwareControl
    };

    explicit JZSerialPort(JZObject *parent = nullptr);
    explicit JZSerialPort(const std::string &name, JZObject *parent = nullptr);
    virtual ~JZSerialPort();

    void setPortName(const std::string &name);
    std::string portName() const;

    // 枚举系统可用串口（委派给驱动）。未安装驱动时返回空列表。
    static std::vector<JZSerialPortInfo> availablePorts();

    // 返回当前端口对应的串口信息（按端口名匹配）。
    JZSerialPortInfo portInfo() const;

    bool open();
    void close();
    bool isOpen() const;

    JZByteArray read(int max_size);
    int read(char *data, int max_size);
    JZByteArray readAll();

    void write(const JZByteArray &buffer);
    void write(const char *data, int len);

    bool setBaudRate(int32_t baudRate);
    int32_t baudRate() const;

    bool setDataBits(DataBits dataBits);
    DataBits dataBits() const;

    bool setParity(Parity parity);
    Parity parity() const;

    bool setStopBits(StopBits stopBits);
    StopBits stopBits() const;

    bool setFlowControl(FlowControl flowControl);
    FlowControl flowControl() const;

    // 硬件控制信号
    bool setDtr(bool level);
    bool setRts(bool level);

    // 由底层驱动异步调用：把收到的数据放入接收缓冲并发出 sigReadyRead
    void onDataReceived(const JZByteArray &data);

jz_signals:
    JZSigVoid sigReadyRead;

protected:
    JZ_DISABLE_COPY(JZSerialPort)

    std::string   m_portName;
    bool          m_open = false;
    int32_t       m_baudRate = Baud9600;
    DataBits      m_dataBits = Data8;
    Parity        m_parity = NoParity;
    StopBits      m_stopBits = OneStop;
    FlowControl   m_flowControl = NoFlowControl;
    JZByteArray   m_readBuffer;
};

#endif // JZSerialPort_H
