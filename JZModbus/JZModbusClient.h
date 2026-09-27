#ifndef JZ_MODBUS_RTU_CLIENT_H_
#define JZ_MODBUS_RTU_CLIENT_H_
#include "JZObject.h"
#include "JZSerialPort.h"
#include "JZTcpSocket.h"
#include <vector>
#include "JZModbusDefine.h"
#include "JZModbusParam.h"

class JZModbusContext;
class JZFRAMEWORK_API JZModebusReply
{
public:
    JZModebusReply();

    int code;
    int function;
    int addr;        
    std::vector<uint8_t> bit;
    std::vector<uint16_t> reg;
};

class JZFRAMEWORK_API JZModbusClient : public JZObject
{
public:
    JZModbusClient(JZObject *parent = nullptr);
    ~JZModbusClient();
   
    JZModbusConnetInfo connInfo();
    void initConn(JZModbusConnetInfo config);
   
    bool open();
    void openAsync();

    void close();
    bool isOpen();
    
    bool setSlave(int salve);
    void setPlcMode(bool flag);

    std::string error();
    bool isBusy();
    bool waitFinish();

    bool readBits(int addr, int nb, std::vector<uint8_t> &dest);
    bool readInputBits(int addr, int nb, std::vector<uint8_t> &dest);
    bool readRegisters(int addr, int nb, std::vector<uint16_t> &dest);
    bool readInputRegisters(int addr, int nb, std::vector<uint16_t> &dest);

    bool readBitsAsync(int addr, int nb);
    bool readInputBitsAsync(int addr, int nb);
    bool readRegistersAsync(int addr, int nb);
    bool readInputRegistersAsync(int addr, int nb);

    bool writeBit(int coil_addr, int status);
    bool writeBits(int addr, const std::vector<uint8_t> &dest);
    bool writeRegister(int reg_addr, int value);
    bool writeRegisters(int addr, const std::vector<uint16_t> &dest);

    bool writeBitAsync(int coil_addr, int status);
    bool writeBitsAsync(int addr, const std::vector<uint8_t> &dest);
    bool writeRegisterAsync(int reg_addr, int value);
    bool writeRegistersAsync(int addr, const std::vector<uint16_t> &dest);

jz_signals:
    JZSigBool sigModbusOpen;
    JZSignal<const JZModebusReply&> sigModbusReplay;

protected:
    void onTcpConnected();
    void onTcpErrorOccurred(const std::string &error);
    void onReadyRead();

protected:
    struct ReqInfo
    {
        void clear();

        int function;
        int addr;
        int nb;        
        int64_t reqTime;
        JZByteArray buffer;
    };

    virtual void timerEvent(JZTimerEvent* e) override;

    void initRtu(std::string com, int baud, JZSerialPort::DataBits, JZSerialPort::StopBits, JZSerialPort::Parity);
    void initTcp(std::string ip, int port);
    void clear();
    bool checkOpenBusy();
    void sendPacket(uint8_t *req, int req_length);
    JZModebusReply waitPacket();        
    int dealBuffer(JZModebusReply &info);
    bool isIo();
    void ioWaitForReadyRead(int ms);
    void ioWrite(const JZByteArray& buffer);
    JZByteArray ioReadAll();
    
    JZModbusContext *m_ctx;    
    ReqInfo m_req;
    JZByteArray m_buffer;
    std::string m_error;
    bool m_waitRecv;
    int m_timeId;

    JZModbusConnetInfo m_conn;

    JZSerialPort *m_com;
    JZTcpSocket *m_socket;
};

#endif // !JZ_MODBUS_CLIENT_H_
