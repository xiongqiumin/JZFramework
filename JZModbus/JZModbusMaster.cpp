#include "JZTimer.h"
#include "JZPlatform.h"
#include "JZModbusMaster.h"

JZModbusMaster::JZModbusMaster()
    :sigModbusOpen(this),sigParamChanged(this),sigParamReceived(this)
{
    m_client = new JZModbusClient(this);
    connect(&m_client->sigModbusReplay, this, &JZModbusMaster::onModbusReply);
    connect(&m_client->sigModbusOpen, this, &JZModbusMaster::onModbusOpen);

    m_timeId = 0;
    m_strategyPause = false;
}

JZModbusMaster::~JZModbusMaster()
{
}

JZModbusParam *JZModbusMaster::param(int addr)
{
    return m_map.param(addr);
}

JZVariant JZModbusMaster::paramValue(int addr)
{    
    auto p = m_map.param(addr);
    JZ_ASSERT(p);

    return p->value;
}

bool JZModbusMaster::writeParam(int addr, JZVariant value)
{
    m_client->waitFinish();

    auto p = m_map.param(addr);
    JZ_ASSERT(p);

    p->value = value;    
    void *ptr = nullptr;
    if (p->addrType == Param_Coil)
        return m_client->writeBit(p->addr, p->value.toInt());        
    else
    {
        std::vector<uint16_t> buffer;
        buffer.resize(p->byteSize()/2);
        p->pack(buffer);
        return m_client->writeRegisters(p->addr, buffer);
    }
}

JZVariant JZModbusMaster::readParam(int addr)
{
    m_client->waitFinish();

    auto p = m_map.param(addr);
    if (!p)
        return JZVariant();
     
    if (p->addrType == Param_Coil)
    {
        std::vector<uint8_t> dest;
        if (m_client->readBits(p->addr, 1, dest))
            p->value = dest[0];        
    }
    else if (p->addrType == Param_DiscreteInput)
    {
        std::vector<uint8_t> dest;
        if (m_client->readInputBits(p->addr, 1, dest))
            p->value = dest[0];
    }
    else
    {
        std::vector<uint16_t> buffer;
        int nb = p->byteSize() / 2;
        bool ret = false;
        if (p->addrType == Param_HoldingRegister)
            ret = m_client->readRegisters(p->addr,nb,buffer);
        else if (p->addrType == Param_InputRegister)
            ret = m_client->readInputRegisters(p->addr, nb, buffer);

        if (ret)
            p->unpack(buffer);
    }
    return p->value;
}

void JZModbusMaster::writeParamAsync(int addr, JZVariant value)
{
    auto p = m_map.param(addr);
    if (!p)
        return;

    Command cmd;
    cmd.type = Command::Write;
    cmd.addr = addr;
    cmd.value = value;
    m_commandList.push_back(cmd);    

    dealCommand();
}

void JZModbusMaster::readParamAsync(int addr)
{
    auto p = m_map.param(addr);
    if (!p)
        return;

    Command cmd;
    cmd.type = Command::Read;
    cmd.addr = addr;    
    m_commandList.push_back(cmd);

    dealCommand();
}

void JZModbusMaster::setSlave(int slave)
{
    m_client->setSlave(slave);
}

void JZModbusMaster::setPlcMode(bool mode)
{
    m_client->setPlcMode(mode);
}

bool JZModbusMaster::isOpen()
{
    return m_client->isOpen();
}

bool JZModbusMaster::isBusy()
{
    return m_client->isBusy();
}

void JZModbusMaster::initConn(JZModbusConnetInfo conn)
{
    m_client->initConn(conn);
}

bool JZModbusMaster::open()
{
    bool ret = m_client->open();
    if (ret && m_timeId == 0)
        m_timeId = startTimer(20);

    return ret;
}

void JZModbusMaster::openAsync()
{
    m_client->openAsync();
    m_timeId = startTimer(20);
}

void JZModbusMaster::close()
{
    m_client->close();
    if (m_timeId != 0)
    {
        killTimer(m_timeId);
        m_timeId = 0;
    }
}

JZModbusParamMap *JZModbusMaster::map()
{
    return &m_map;
}

void JZModbusMaster::setStrategy(int addr, JZModbusStrategy s)
{
    JZ_ASSERT(m_map.contains(addr));
    
    Strategy info;
    info.s = s;
    info.readTime = 0;
    m_strategyMap[addr] = info;
}

void JZModbusMaster::removeStrategy(int addr)
{
    map_remove(m_strategyMap, addr);
}

void JZModbusMaster::pauseStrategy()
{
    m_strategyPause = true;
}

void JZModbusMaster::resumeStrategy()
{
    m_strategyPause = false;
}

void JZModbusMaster::onModbusOpen(bool flag)
{
    sigModbusOpen.emit(flag);
}

void JZModbusMaster::onModbusReply(const JZModebusReply &reply)
{
    if (reply.code != 0)
        return;

    if (reply.function == MODBUS_FC_READ_COILS || reply.function == MODBUS_FC_READ_DISCRETE_INPUTS
        || reply.function == MODBUS_FC_READ_HOLDING_REGISTERS || reply.function == MODBUS_FC_READ_INPUT_REGISTERS)
    {
        bool is_bit = (reply.function == MODBUS_FC_READ_COILS || reply.function == MODBUS_FC_READ_DISCRETE_INPUTS);
        int nb = is_bit ? reply.bit.size() : reply.reg.size();

        auto list = m_map.paramList();        
        int offset = 0;
        for (int addr = reply.addr; addr < reply.addr + nb; addr++, offset++)
        {
            for (int i = 0; i < list.size(); i++)
            {
                if (addr == list[i])
                {
                    auto param = m_map.param(addr);
                    JZVariant pre = param->value;
                    if (is_bit)
                    {
                        param->value = reply.bit[offset];
                    }
                    else
                    {
                        auto buffer = vec_mid(reply.reg,offset, param->byteSize() / 2);
                        param->unpack(buffer);
                    }

                    auto it = m_strategyMap.find(addr);
                    if (it != m_strategyMap.end())
                    {
                        if (it->second.s.recvNotify)
                            sigParamReceived.emit(addr);
                    }

                    if (pre != param->value)
                        sigParamChanged.emit(addr);
                }
            }
        }
    }
    dealCommand();
}

void JZModbusMaster::timerEvent(JZTimerEvent *e)
{
    if (!m_client->isOpen())
        return;

    dealCommand();
}

void JZModbusMaster::dealCommand()
{
    if (!m_client->isOpen() || m_client->isBusy())
        return;

    if (m_commandList.size() > 0)
    {
        auto cmd = m_commandList[0];
        vec_popFront(m_commandList);
        
        auto p = m_map.param(cmd.addr);
        if (cmd.type == Command::Write)
        {
            p->setValue(cmd.value);
            if (p->addrType == Param_Coil)
                m_client->writeBitAsync(p->addr, p->value.toInt());
            else
            {
                std::vector<uint16_t> buffer;
                buffer.resize(p->byteSize() / 2);
                p->pack(buffer);
                m_client->writeRegistersAsync(p->addr, buffer);
            }        
        }
        else if(cmd.type == Command::Read)
        {
            if (p->addrType == Param_Coil)
            {
                m_client->readBitsAsync(p->addr, 1);
            }
            else if (p->addrType == Param_DiscreteInput)
            {
                m_client->readInputBitsAsync(p->addr, 1);
            }
            else
            {
                std::vector<uint16_t> buffer;
                int nb = p->byteSize() / 2;                
                if (p->addrType == Param_HoldingRegister)
                    m_client->readRegistersAsync(p->addr, nb);
                else if (p->addrType == Param_InputRegister)
                    m_client->readInputRegistersAsync(p->addr, nb);
            }
        }
        return;
    }

    int64_t cur_ms = JZPlatform::getTickCountMs();
    if (!m_strategyPause)
    {
        auto it = m_strategyMap.begin();
        while (it != m_strategyMap.end())
        {           
            if (it->second.s.autoRead && it->second.readTime + it->second.s.autoReadTime < cur_ms)
            {
                auto param = m_map.param(it->first);
                int nb = param->byteSize() / 2;
                m_client->readRegistersAsync(param->addr, nb);
                it->second.readTime = cur_ms;
                break;
            }
            it++;
        }
    }
}

void modbusMasterSetConfig(JZModbusMaster *master, const JZModbusConfig *c)
{
    auto map = master->map();
    map->clear();

    for (int i = 0; i < c->paramList.size(); i++)
        map->add(c->paramList[i]);

    master->initConn(c->conn);

    auto it = c->strategyMap.begin();
    while (it != c->strategyMap.end())
    {
        master->setStrategy(it->first, it->second);
        it++;
    }
}