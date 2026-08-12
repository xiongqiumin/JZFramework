#ifndef JZ_SIMULATOR_CLIENT_H_
#define JZ_SIMULATOR_CLIENT_H_

#include "JZObject.h"
#include "JZNetClient.h"
#include "JZNetPacket.h"
#include "JZSimPack.h"
#include <string>

// 模拟器统一通信客户端(单例)。
// 所有模拟驱动(BLE / GPIO / WiFi ...)共享同一个 JZNetClient 连接到 JZSimulatorDevice,
// 不再各自创建连接。客户端收包后按 JZSimCmd 的模块段主动路由到对应驱动的 onRecv()。
class JZSimulatorClient : public JZObject
{
    JZ_OBJECT_DECLARE(JZSimulatorClient)
public:
    static JZSimulatorClient *instance();
    static void setInstance(JZSimulatorClient *inst);

    JZSimulatorClient(const std::string &ip = "127.0.0.1", int port = 19123);
    ~JZSimulatorClient() override;

    bool isConnected() const;
    void sendSim(JZSimCmd cmd, const JZVariantMap &data);

private:
    void onClientPacket(JZNetPackPtr pack);

    JZNetClient *m_client = nullptr;
    static JZSimulatorClient *m_inst;
};

#endif // !JZ_SIMULATOR_CLIENT_H_
