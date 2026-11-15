#include "JZInprocCli.h"

#include "JZAgentCli.h"
#include "JZAgentCliTask.h"    // JZCliConfig
#include "JZCliConfigIO.h"     // jzCliLoadConfig(与独立 exe 同一份解析)
#include "JZDebug.h"

JZInprocCli::JZInprocCli()
{
}

JZInprocCli::~JZInprocCli()
{
    // 析构时还活着就先停下来: 线程里持有 JZAgentCli, 不能让它继续跑
    stop();
}

bool JZInprocCli::startCli(const std::string &configPath, const std::string &agentName,
                           const std::string &hubHost, int hubPort, const std::string &hubToken,
                           std::string &error)
{
    // 配置解析放在调用线程: 这里只是读文件 + 填结构体, 不碰任何 JZ 对象,
    // 出错能同步报给调用方(放进线程里就只能靠日志猜)。
    JZCliConfig config;
    if (!jzCliLoadConfig(configPath, config, error))
        return false;

    // inproc 的 cli 必须连**本机**这台 server: 配置里写的 server.* 一律覆盖掉,
    // 否则会出现"进程内跑着, 却连到别的服务器"这种事故。
    config.server.host = hubHost;
    config.server.port = hubPort;
    config.server.token = hubToken;
    if (config.server.agentId.empty())
        config.server.agentId = agentName;

    m_configPath = configPath;
    m_agentName = agentName;
    m_hubHost = hubHost;
    m_hubPort = hubPort;
    m_hubToken = hubToken;

    // 注意: JZAgentCli 不在这个线程建 —— 它归线程自己的事件循环(见 run())
    setLastError(std::string());
    JZThread::start();
    return true;
}

void JZInprocCli::stop()
{
    if (!isRunning())
        return;
    quit();       // 置 m_running=false 并唤醒事件循环
    wait();       // 线程函数 join 后才算真的收干净(JZAgentCli 已在 run() 末尾析构)
}

std::string JZInprocCli::lastError()
{
    JZMutexLocker lock(&m_errorMutex);
    return m_error;
}

void JZInprocCli::setLastError(const std::string &error)
{
    JZMutexLocker lock(&m_errorMutex);
    m_error = error;
}

void JZInprocCli::run()
{
    // ===== 已经在本线程上: 从这里开始建的 JZ 对象都归属这个线程 =====
    JZCliConfig config;
    std::string error;
    if (!jzCliLoadConfig(m_configPath, config, error))
    {
        setLastError("读配置失败: " + error);
        jzError() << "inproc cli 读配置失败:" << error.c_str();
        return;   // 直接返回: 线程函数随后会置 finished, 事件循环根本没起来
    }
    config.server.host = m_hubHost;
    config.server.port = m_hubPort;
    config.server.token = m_hubToken;
    if (config.server.agentId.empty())
        config.server.agentId = m_agentName;

    JZAgentCli *cli = new JZAgentCli();
    if (!cli->init(config, error))
    {
        setLastError("初始化失败: " + error);
        jzError() << "inproc cli 初始化失败:" << error.c_str();
        delete cli;
        return;
    }
    m_cli = cli;

    // 与 main.cpp 里独立 exe 的启动顺序保持一致
    cli->startMcp();
    cli->startNodes();
    cli->startServerLink();    // managed: 连本机 hub(地址已在上面覆盖)

    JZThread::run();           // 进本线程的事件循环, 直到 stop() -> quit()

    // 事件循环已退出: 在本线程析构(定时器 / 信号都在这个线程上)
    m_cli = nullptr;
    delete cli;
}
