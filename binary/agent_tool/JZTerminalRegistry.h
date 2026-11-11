#ifndef JZ_TERMINAL_REGISTRY_H_
#define JZ_TERMINAL_REGISTRY_H_

#include <string>
#include <vector>

#include "JZVirtualTerminal.h"

// 终端注册表: 把"会话 id"映射到具体 JZVirtualTerminal 实例。
//
// 放在 agent_tool 层(不反向依赖 agent_node), 让 MCP 工具只依赖这个接口、
// 不与具体终端绑定(满足"JZTerminalRunMcpTool 不要和 JZVirtualTerminal 绑定起来")。
// 由 agent_node 用 map<session, JZVirtualTerminal> 实现: 每个会话一个终端实例,
// 生命周期(创建/销毁/空闲回收)由 node 管。
class JZTerminalRegistry
{
public:
    virtual ~JZTerminalRegistry() = default;

    // 按会话 id 取终端; 不存在返回 nullptr(并填 error)
    virtual JZVirtualTerminal *terminal(const std::string &sessionId, std::string &error) = 0;

    // 新建一个会话(可选 cwd), 返回会话 id; 失败返回空
    virtual std::string open(const std::string &cwd, std::string &error) = 0;

    // 关闭会话(销毁对应终端, 在跑命令会被终止); 成功 true
    virtual bool close(const std::string &sessionId) = 0;

    // 列出会话(id / cwd / 是否运行中)
    virtual std::vector<JZVirtualTerminal::SessionInfo> list() = 0;
};

#endif // !JZ_TERMINAL_REGISTRY_H_
