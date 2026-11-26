#ifndef JZ_SERVER_EXTRA_TOOLS_H_
#define JZ_SERVER_EXTRA_TOOLS_H_

class JZAgentCliServer;

// 把 server 控制面工具注册成配置驱动的工具。
// 只在 server 进程启动时调用一次; 工具本身跑在 cli 线程, 通过异步代理访问 server 线程。
void jzRegisterServerExtraTools(JZAgentCliServer *server);
void jzUnregisterServerExtraTools();

#endif // !JZ_SERVER_EXTRA_TOOLS_H_
