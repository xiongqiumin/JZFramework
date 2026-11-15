#ifndef JZ_INPROC_CLI_H_
#define JZ_INPROC_CLI_H_

#include <string>

#include "JZMutex.h"
#include "JZThread.h"

class JZAgentCli;

// ---------------------------------------------------------------------------
// JZInprocCli —— 把一台 agent_cli 跑在**本进程的一个线程**里(源码引用, 不 exec 子进程)
//
// 为什么这么做:
//   * 子进程方案要自己处理"父死子活"的孤儿问题(Linux 没有 Windows 那种作业对象),
//     还得为每台 cli 付一份进程开销、跨进程收集日志;
//   * agent_cli 本身就是 JZObject + 事件循环, 直接跑在独立线程里即可 —— 卡死的任务
//     也压不住 server 的主循环(管理台/控制面照常响应)。
//
// 线程模型:
//   JZThread 子类。JZAgentCli / JZHubLink / 终端等所有 JZ 对象都在 run() 里创建,
//   也就是**都归属这个线程** —— 定时器"只能在自己线程上建"的约束天然满足。
//
// 与 server 的关系:
//   仍然走控制面(hub)连回本机(127.0.0.1), 只是不再有进程边界。任务下发 / 事件回流 /
//   agent 列表 / token 校验全部复用现有链路, 不另开旁路。
// ---------------------------------------------------------------------------
class JZInprocCli : public JZThread
{
public:
    JZInprocCli();
    ~JZInprocCli() override;

    // 准备并启动线程。configPath = agent_cli 的配置文件(与独立 exe 用的那份完全一样);
    // hubHost/hubPort/hubToken = 本机控制面, 会**覆盖**配置里的 server.*(inproc 的 cli
    // 只能连自己这台 server)。配置解析在调用线程上做, 出错直接返回 false。
    bool startCli(const std::string &configPath, const std::string &agentName,
                  const std::string &hubHost, int hubPort, const std::string &hubToken,
                  std::string &error);

    // 退出事件循环并等线程结束(可重复调用)
    void stop();

    // 线程内初始化失败的原因(空 = 没问题)
    std::string lastError();

protected:
    void run() override;

private:
    void setLastError(const std::string &error);

    std::string m_configPath;
    std::string m_agentName;
    std::string m_hubHost;
    int m_hubPort = 0;
    std::string m_hubToken;

    JZAgentCli *m_cli = nullptr;    // 只在 cli 线程上创建 / 析构

    JZMutex m_errorMutex;
    std::string m_error;
};

#endif
