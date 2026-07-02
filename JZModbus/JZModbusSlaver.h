#ifndef JZ_MODBUS_SLAVE_H_
#define JZ_MODBUS_SLAVE_H_

#include "JZObject.h"
#include "JZModbusParam.h"
#include "JZModbusServer.h"

class JZModbusSlaver : public JZObject
{
public:
    JZModbusSlaver(JZObject *parent = nullptr);
    ~JZModbusSlaver();

    void initConn(JZModbusConnetInfo conn);
    void setSlave(int slave);    

    bool isStart() const;
    bool startServer();
    void stopServer();

    JZModbusParamMap *map();
    void initMapping();

    void writeParam(int addr, JZVariant value);
    JZVariant readParam(int addr);

jz_signals:
    JZSigInt sigParamChanged;
    JZSigInt sigParamRecved;

protected:
    void onMappingRecved(int addr, int nb);

protected:
    JZModbusParamMap m_map;
    JZModbusServer *m_server;

};
void modbusSlaverSetConfig(JZModbusSlaver *slaver, const JZModbusConfig *cfg);

#endif