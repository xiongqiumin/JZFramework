#ifndef JZSerialPort_H
#define JZSerialPort_H

#include "JZObject.h"
#include "JZSerialPortInfo.h"

class JZSerialPortPrivate;
class JZSerialPort : public JZObject
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
    explicit JZSerialPort(const JZSerialPortInfo &info, JZObject *parent = nullptr);
    virtual ~JZSerialPort();

    void setPortName(const std::string &name);
    std::string portName() const;

    void setPort(const JZSerialPortInfo &info);

    bool open();
    void close();
    bool isOpen();

	JZByteArray read(int max_size);
	int read(char *size,int max_size);
    JZByteArray readAll();
	
    void write(const JZByteArray &buffer);
    void write(const char *size,int len);

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

protected:
    JZ_DISABLE_COPY(JZSerialPort)
	
	JZSerialPortPrivate *d;
};

#endif // JZSerialPort_H
