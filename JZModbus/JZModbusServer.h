#ifndef JZ_MODBUS_SERVER_H_
#define JZ_MODBUS_SERVER_H_

#include <map>
#include "JZObject.h"
#include "JZSerialPort.h"
#include "JZTcpSocket.h"
#include "JZTcpServer.h"
#include "JZModbusDefine.h"
#include "JZModbusParam.h"

class JZModbusContext;
class JZModbusServer : public JZObject
{
public:
    JZModbusServer(JZObject *parent = nullptr);
    ~JZModbusServer();

    void initMapping(int nb_bits, int nb_input_bits,
        int nb_registers, int nb_input_registers);
    JZModbusMapping *mapping();

    void setSlave(int slave);
    void setPlcMode(bool slave);

    void initConn(JZModbusConnetInfo conn);
    bool isStart() const;
    bool start();
    void stop();

protected:
    void onComRead();
    void onNewConnect(JZTcpSocket* socket);
    void onTcpRead();
    void onTcpDisconnected();

jz_signals:
    JZSignal<int,int> sigMappingRecved;

protected:    
    void initRtu(std::string com, int baud, JZSerialPort::DataBits, JZSerialPort::StopBits, JZSerialPort::Parity);
    void initTcp(int port);

    int dealBuffer(uint8_t *req,JZByteArray &buffer);
    void clear();

    JZModbusContext *m_ctx;
    
    std::string m_comName;
    int m_baud;
    JZSerialPort::DataBits m_dataBit;
    JZSerialPort::StopBits m_stopBit;
    JZSerialPort::Parity m_parityBit;
    int m_port;

    JZSerialPort *m_com;
    JZByteArray m_comBuffer;
    
    JZTcpServer *m_tcpServer;
    std::map<JZTcpSocket*, JZByteArray> m_tcpClients;
    JZModbusMapping *m_mapping;
};

#endif