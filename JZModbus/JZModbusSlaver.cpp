#include <memory.h>
#include <climits>
#include "JZModbusSlaver.h"

JZModbusSlaver::JZModbusSlaver(JZObject *parent)
    :JZObject(parent), sigParamChanged(this), sigParamRecved(this)
{
    m_server = new JZModbusServer(this);
    connect(&m_server->sigMappingRecved, this, &JZModbusSlaver::onMappingRecved);
}

JZModbusSlaver::~JZModbusSlaver()
{
}

void JZModbusSlaver::initConn(JZModbusConnetInfo conn)
{
    m_server->initConn(conn);
}

void JZModbusSlaver::setSlave(int slave)
{
    m_server->setSlave(slave);
}

JZModbusParamMap *JZModbusSlaver::map()
{
    return &m_map;
}

bool JZModbusSlaver::isStart() const
{
    return m_server->isStart();
}

bool JZModbusSlaver::startServer()
{
    return m_server->start();
}

void JZModbusSlaver::stopServer()
{
    m_server->stop();
}

void JZModbusSlaver::initMapping()
{    
    int start_bits = INT_MAX;
    int end_bits = 0;
    int start_input_bits = INT_MAX;
    int end_input_bits = 0;
    int start_input_registers = INT_MAX;
    int end_input_registers = 0;
    int start_registers = INT_MAX;
    int end_registers = 0;

    auto list = m_map.paramList();
    for (int i = 0; i < list.size(); i++)
    {
        auto p = m_map.param(list[i]);
        if(p->addrType == Param_Coil)
        {
            start_bits = std::min(start_bits, p->addr);
            end_bits = std::max(end_bits, p->addr + 1);
        }
        else if(p->addrType == Param_DiscreteInput)
        {
            start_input_bits = std::min(start_input_bits, p->addr);
            end_input_bits = std::max(end_input_bits, p->addr + 1);            
        }
        else if(p->addrType == Param_HoldingRegister)
        {
            int nb = p->byteSize() / 2;
            start_registers = std::min(start_registers, p->addr);
            end_registers = std::max(end_registers, p->addr + nb);            
        }
        else if(p->addrType == Param_InputRegister)
        {
            int nb = p->byteSize() / 2;
            start_input_registers = std::min(start_input_registers, p->addr);
            end_input_registers = std::max(end_input_registers, p->addr + nb);
        }
    }

    int nb_bits = (end_bits > start_bits) ? end_bits - start_bits : 0;
    int nb_input_bits = (end_input_bits > start_input_bits) ? end_input_bits - start_input_bits : 0;
    int nb_input_registers = (end_input_registers > start_input_registers) ? end_input_registers - start_input_registers : 0;
    int nb_registers = (end_registers > start_registers) ? end_registers - start_registers : 0;

    auto map = m_server->mapping();
    map->init(nb_bits, nb_input_bits, nb_registers, nb_input_registers);
    map->start_bits = start_bits;
    map->start_input_bits = start_input_bits;
    map->start_registers = start_registers;
    map->start_input_registers = start_input_registers;
    
    for (int i = 0; i < list.size(); i++)
    {
        auto p = m_map.param(list[i]);
        writeParam(p->addr, p->value);
    }
}

void JZModbusSlaver::writeParam(int addr, JZVariant value)
{
    auto p = m_map.param(addr);
    JZ_ASSERT(p);

    p->setValue(value);
    auto map = m_server->mapping();    
    if (p->addrType == Param_Coil || p->addrType == Param_DiscreteInput)
    {
        if (p->addrType == Param_Coil)
        {
            int mapping_address = addr - map->start_bits;
            map->tab_bits[mapping_address] = (value.toInt() != 0)? 1 : 0;
        }
        else if (p->addrType == Param_DiscreteInput)
        {
            int mapping_address = addr - map->start_input_bits;
            map->tab_input_bits[mapping_address] = (value.toInt() != 0)? 1 : 0;
        }
    }
    else
    {
        std::vector<uint16_t> buffer;
        p->pack(buffer);

        uint16_t *ptr = nullptr;
        if (p->addrType == Param_HoldingRegister)
        {
            int mapping_address = addr - map->start_registers;
            ptr = map->tab_registers + mapping_address;
        }
        else if (p->addrType == Param_InputRegister)
        {
            int mapping_address = addr - map->start_input_registers;
            ptr = map->tab_input_registers + mapping_address;
        }
        else {
            JZ_ASSERT(0);
        }

        for (int i = 0; i < buffer.size(); i++)
            ptr[i] = buffer[i];
    }    
}

JZVariant JZModbusSlaver::readParam(int addr)
{
    auto p = m_map.param(addr);
    JZ_ASSERT(p);

    return p->value;
}

void JZModbusSlaver::onMappingRecved(int in_addr, int nb)
{
    auto map = m_server->mapping();
    auto list = m_map.paramList();    
    for (int addr = in_addr; addr < in_addr + nb; addr++)
    {
        for (int i = 0; i < list.size(); i++)
        {
            if (addr == list[i])
            {                
                auto p = m_map.param(addr);                
                JZVariant pre = p->value;

                if (p->addrType == Param_Coil)
                {
                    auto ptr = map->tab_bits + addr - map->start_bits;
                    p->value = *ptr;
                }
                else if (p->addrType == Param_HoldingRegister)
                {
                    auto ptr = map->tab_registers + addr - map->start_registers;   
                    std::vector<uint16_t> buffer;
                    for (int i = 0; i < p->byteSize() / 2; i++)
                        buffer.push_back(ptr[i]);

                    p->unpack(buffer);
                }

                if (pre != p->value)
                    sigParamChanged.emit(addr);

                sigParamRecved.emit(addr);
            }
        }
    }
}

void modbusSlaverSetConfig(JZModbusSlaver *slaver, const JZModbusConfig *c)
{
    auto map = slaver->map();
    map->clear();
    for (int i = 0; i < c->paramList.size(); i++)
        map->add(c->paramList[i]);
    slaver->initMapping();
    slaver->initConn(c->conn);
}