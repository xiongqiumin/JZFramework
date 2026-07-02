#ifndef JZ_MODBUS_PARAM_H_
#define JZ_MODBUS_PARAM_H_

#include <inttypes.h>
#include <vector>
#include "JZVariant.h"
#include "JZDataStream.h"
#include "JZSerialPort.h"

enum {
    Modbus_rtuClient,
    Modbus_tcpClient,
    Modbus_rtuServer,
    Modbus_tcpServer,
};

enum {
    Param_Coil,
    Param_DiscreteInput,
    Param_HoldingRegister,
    Param_InputRegister,
};

class JZModbusStrategy
{
public:
    JZModbusStrategy();

    bool recvNotify;
    bool autoRead;
    int autoReadTime;
};
JZDataStream &operator<<(JZDataStream &s, const JZModbusStrategy &param);
JZDataStream &operator>>(JZDataStream &s, JZModbusStrategy &param);

class JZModbusParam
{
public:
    enum {
        Data_bit,
        Data_int16,
        Data_uint16,
        Data_int32,
        Data_uint32,
        Data_int64,
        Data_uint64,
        Data_float,
        Data_double,
    };

    static std::vector<std::string> addrTypeList();
    static std::vector<std::string> dataTypeList();

    JZModbusParam();

    int byteSize() const;
    int addrEnd() const;
    void setValue(const JZVariant &v);

    void unpack(const std::vector<uint16_t>& buffer);
    void pack(std::vector<uint16_t>& buffer);

    std::string name;
    int addr;
    int addrType;
    int dataType;
    JZVariant value;
    JZDataStream::ByteOrder byteOrder;  //远端的字节序，本地总是小端
    std::string memo;  
};
JZDataStream &operator<<(JZDataStream &s, const JZModbusParam &param);
JZDataStream &operator>>(JZDataStream &s, JZModbusParam &param);

class JZModbusParamMap
{
public:
    void clear();

    bool add(const JZModbusParam &value);
    void remove(int addr);
    JZModbusParam *param(int addr);    
    std::vector<int> paramList();
    bool contains(int addr);
    int count();        

protected:
    int indexOf(int addr);

    std::vector<JZModbusParam> m_params;
};

//JZModbusConnetInfo
class JZModbusConnetInfo
{
public:
    JZModbusConnetInfo();

    bool isRtu() const;
    bool isMaster() const;

    int modbusType;
    int slave;
    bool plcMode;

    std::string portName;
    int baud;
    JZSerialPort::DataBits dataBit;
    JZSerialPort::Parity parityBit;
    JZSerialPort::StopBits stopBit;

    std::string ip;
    int port;
};
JZDataStream &operator<<(JZDataStream &s, const JZModbusConnetInfo &param);
JZDataStream &operator>>(JZDataStream &s, JZModbusConnetInfo &param);

//JZModbusConfig
class JZModbusConfig
{
public:
    JZModbusConfig();

    bool isRtu() const;
    bool isMaster() const;

    JZModbusConnetInfo conn;
    std::map<int,JZModbusStrategy> strategyMap;
    std::vector<JZModbusParam> paramList;    
};
JZDataStream &operator<<(JZDataStream &s, const JZModbusConfig &param);
JZDataStream &operator>>(JZDataStream &s, JZModbusConfig &param);

#endif // !JZ_MODBUS_VALUE_H_
