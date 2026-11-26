#ifndef JZ_CLI_EXTRA_TOOL_H_
#define JZ_CLI_EXTRA_TOOL_H_

#include <functional>
#include <string>

class JZAgentTool;
class JZObject;

// 配置驱动的额外工具注册表。
//   server 这类宿主在进程启动时把自己的工具工厂注册进来;
//   cli 配置里的 extraTools[] / agents[].tools[] 按名字在这里创建工具实例。
using JZCliExtraToolFactory = std::function<JZAgentTool *(JZObject *parent)>;

void jzRegisterCliExtraTool(const std::string &name, const JZCliExtraToolFactory &factory);
void jzUnregisterCliExtraTool(const std::string &name);
JZAgentTool *jzCreateCliExtraTool(const std::string &name, JZObject *parent);

#endif // !JZ_CLI_EXTRA_TOOL_H_
