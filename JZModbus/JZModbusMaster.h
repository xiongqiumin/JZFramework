#ifndef JZ_MODBUS_MASTER_H_
#define JZ_MODBUS_MASTER_H_
#include <map>
#include "JZObject.h"
#include "JZModbusParam.h"
#include "JZModbusClient.h"

class JZFRAMEWORK_API JZModbusMaster : public JZObject
{
public:
    JZModbusMaster();
    ~JZModbusMaster();    
    
    void setSlave(int slave);
    void setPlcMode(bool mode);

    bool isOpen();
    bool isBusy();

    void initConn(JZModbusConnetInfo conn);
    bool open();
    void openAsync();
    void close();    
    
    JZModbusParamMap *map();
    void setStrategy(int addr, JZModbusStrategy s);
    void removeStrategy(int addr);
    void pauseStrategy();
    void resumeStrategy();

    JZModbusParam *param(int addr);    
    JZVariant paramValue(int addr);

    bool writeParam(int addr, JZVariant value);
    JZVariant readParam(int addr);

    void writeParamAsync(int addr, JZVariant value);
    void readParamAsync(int addr);

jz_signals:
    JZSigBool sigModbusOpen;
    JZSigInt sigParamChanged;
    JZSigInt sigParamReceived;

protected:
    void onModbusReply(const JZModebusReply &reply);
    void onModbusOpen(bool flag);
    
protected:
    struct Command
    {
        enum {
            Read,
            Write,
        };

        int type;
        int addr;
        JZVariant value;
    };

    struct Strategy
    {
        JZModbusStrategy s;
        int64_t readTime;
    };

    virtual void timerEvent(JZTimerEvent *e) override;
    void dealCommand();    

    JZModbusParamMap m_map;
    JZModbusClient *m_client;
    int m_timeId;
    std::map<int, Strategy> m_strategyMap;
    std::vector<Command> m_commandList;
    bool m_strategyPause;
};
void modbusMasterSetConfig(JZModbusMaster *master, const JZModbusConfig *config);

#endif