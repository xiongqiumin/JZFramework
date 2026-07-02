#include "JZDataStream.h"
#include "JZModbusParam.h"


//JZModbusStrategy
JZModbusStrategy::JZModbusStrategy()
{    
    recvNotify = false;
    autoRead = false;
    autoReadTime = 1000;
}

JZDataStream &operator<<(JZDataStream &s, const JZModbusStrategy &param)
{
    s << param.autoRead << param.autoReadTime << param.recvNotify;
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZModbusStrategy &param)
{
    s >> param.autoRead >> param.autoReadTime >> param.recvNotify;    
    return s;
}

//JZModbusParam
std::vector<std::string> JZModbusParam::addrTypeList()
{
    std::vector<std::string> list = { "Coil","DiscreteInput","HoldingRegister","InputRegister" };
    return list;
}

std::vector<std::string> JZModbusParam::dataTypeList()
{
    std::vector<std::string> list = { "bit","int16","int32","float","double" };
    return list;
}

JZModbusParam::JZModbusParam()
{
    addr = 0;
    addrType = Param_HoldingRegister;
    dataType = Data_int16;
    byteOrder = JZDataStream::LittleEndian;
    value = 0;
}

int JZModbusParam::addrEnd() const
{
    if (addrType == Param_HoldingRegister || addrType == Param_InputRegister)
        return addr + byteSize() / 2;
    else
        return addr + 1;
}

void JZModbusParam::setValue(const JZVariant &v)
{
    value = v;
}

int JZModbusParam::byteSize() const
{
    int size = 0;
    if (dataType == Data_bit)
        size = 1;
    else if (dataType == Data_int16 || dataType == Data_uint16)
        size = 2;
    else if (dataType == Data_int32 || dataType == Data_uint32)
        size = 4;
    else if (dataType == Data_int64 || dataType == Data_uint64)
        size = 4;
    else if (dataType == Data_float)
        size = 4;
    else if (dataType == Data_double)
        size = 8;

    return size;
}

template<class T> 
static void paramUnpack(JZModbusParam *p, const std::vector<uint16_t>& buffer)
{
    T value = 0;
    const char *ptr = (const char *)buffer.data();
    JZByteArray bytes = JZByteArray::fromRawData(ptr, (int)buffer.size() * 2);
    JZDataStream s(&bytes, JZ::ReadOnly);
    s.setByteOrder(p->byteOrder);

    s >> value;
    p->value = JZVariant::fromValue<T>(value);
}

void JZModbusParam::unpack(const std::vector<uint16_t>& buffer)
{
    if (dataType == Data_int16)
        paramUnpack<int16_t>(this, buffer);
    else if (dataType == Data_uint16)
        paramUnpack<uint16_t>(this, buffer);
    else if (dataType == Data_int32)
        paramUnpack<int32_t>(this, buffer);
    else if (dataType == Data_uint32)
        paramUnpack<uint32_t>(this, buffer);
    else if (dataType == Data_int64)
        paramUnpack<int64_t>(this, buffer);
    else if (dataType == Data_uint64)
        paramUnpack<uint64_t>(this, buffer);
    else if (dataType == Data_float)
        paramUnpack<float>(this, buffer);
    else if (dataType == Data_double)
        paramUnpack<double>(this, buffer);
}

template<class T>
static void paramPack(JZModbusParam* p, std::vector<uint16_t>& out)
{
    T value = p->value.value<T>();    

    JZByteArray bytes;
    JZDataStream s(&bytes, JZ::WriteOnly);
    s.setByteOrder(p->byteOrder);

    s << value;

    out.resize(sizeof(T) / 2);
    memcpy(out.data(), bytes.data(), bytes.size() );
}

void JZModbusParam::pack(std::vector<uint16_t>& buffer)
{
    if (dataType == Data_int16)
        paramPack<int16_t>(this, buffer);
    else if (dataType == Data_uint16)
        paramPack<uint16_t>(this, buffer);
    else if (dataType == Data_int32)
        paramPack<int32_t>(this, buffer);
    else if (dataType == Data_uint32)
        paramPack<uint32_t>(this, buffer);
    else if (dataType == Data_int64)
        paramPack<int64_t>(this, buffer);
    else if (dataType == Data_uint64)
        paramPack<uint64_t>(this, buffer);
    else if (dataType == Data_float)
        paramPack<float>(this, buffer);
    else if (dataType == Data_double)
        paramPack<double>(this, buffer);
}

JZDataStream &operator<<(JZDataStream &s, const JZModbusParam &param)
{
    s << param.name;
    s << param.addr;
    s << param.addrType;
    s << param.dataType;
    s << param.value;
    s << param.memo;
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZModbusParam &param)
{
    s >> param.name;
    s >> param.addr;
    s >> param.addrType;
    s >> param.dataType;
    s >> param.value;
    s >> param.memo;
    return s;
}

//JZModbusParamMap
void JZModbusParamMap::clear()
{
    m_params.clear();
}

bool JZModbusParamMap::add(const JZModbusParam &value)
{
    int type = value.addrType;
    int addr = value.addr;
    int addr_end = value.addrEnd();
    for (int i = 0; i < m_params.size(); i++)
    {
        auto &p = m_params[i];
        if (p.addrType == type)
        {
            if(p.addr < addr_end && p.addrEnd() > addr)
                return false;
        }
    }

    m_params.push_back(value);
    return true;
}

void JZModbusParamMap::remove(int addr)
{
    int idx = indexOf(addr);
    if (idx == -1)
        return;
    
    vec_removeAt(m_params, idx);
}

JZModbusParam *JZModbusParamMap::param(int addr)
{
    int idx = indexOf(addr);
    if (idx == -1)
        return nullptr;

    return &m_params[idx];
}

int JZModbusParamMap::indexOf(int addr)
{
    for (int i = 0; i < m_params.size(); i++)
    {
        if (m_params[i].addr == addr)
            return i;
    }
    return -1;
}

std::vector<int> JZModbusParamMap::paramList()
{
    std::vector<int> ret;
    for (int i = 0; i < m_params.size(); i++)
        ret.push_back(m_params[i].addr);
    return ret;
}

int JZModbusParamMap::count()
{
    return m_params.size();
}

bool JZModbusParamMap::contains(int addr)
{
    return indexOf(addr) >= 0;
}

//JZModbusConnetInfo
JZModbusConnetInfo::JZModbusConnetInfo()
{
    modbusType = Modbus_rtuClient;
    portName = "COM1";
    baud = 9600;
    dataBit = JZSerialPort::Data8;
    parityBit = JZSerialPort::NoParity;
    stopBit = JZSerialPort::OneStop;
    slave = 1;    
    ip = "127.0.0.1";
    port = 502;
    plcMode = false;
}

bool JZModbusConnetInfo::isRtu() const
{
    return (modbusType == Modbus_rtuClient || modbusType == Modbus_rtuServer);
}

bool JZModbusConnetInfo::isMaster() const
{
    return (modbusType == Modbus_rtuClient || modbusType == Modbus_tcpClient);
}

JZDataStream &operator<<(JZDataStream &s, const JZModbusConnetInfo &param)
{
    s << param.modbusType;
    s << param.slave;
    s << param.plcMode;

    s << param.portName;
    s << param.baud << param.dataBit << param.parityBit << param.stopBit;
    s << param.ip;
    s << param.port;
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZModbusConnetInfo &param)
{
    s >> param.modbusType;
    s >> param.slave;
    s >> param.plcMode;

    s >> param.portName;
    s >> param.baud >> param.dataBit >> param.parityBit >> param.stopBit;
    s >> param.ip;
    s >> param.port;
    return s;
}

//JZModbusConfig
JZModbusConfig::JZModbusConfig()
{    
}

bool JZModbusConfig::isRtu() const
{
    return conn.isRtu();
}

bool JZModbusConfig::isMaster() const
{
    return conn.isMaster();
}

JZDataStream &operator<<(JZDataStream &s, const JZModbusConfig &param)
{
    s << param.conn;
    s << param.strategyMap;
    s << param.paramList;
    return s;
}

JZDataStream &operator>>(JZDataStream &s, JZModbusConfig &param)
{
    s >> param.conn;
    s >> param.strategyMap;
    s >> param.paramList;
    return s;
}