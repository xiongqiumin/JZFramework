#ifndef JZ_CLI_CONFIG_IO_H_
#define JZ_CLI_CONFIG_IO_H_

#include <string>

#include "JZAgentCliTask.h"   // JZCliConfig

// 读 agent_cli 的配置文件(config.json / agent1.json ...)。
//   - 缺项 / 缺分组: 保持 JZCliConfig 里的默认值
//   - 类型不对: 记错并返回 false(不会带着一半默认值静默启动)
// 从 main.cpp 抽出来的原因: agent_cli_server 的 inproc 模式把 cli 跑在**同进程的线程**里
// (源码引用, 不再 exec agent_cli.exe), 那条路径必须与独立 exe 用完全同一套解析。
bool jzCliLoadConfig(const std::string &path, JZCliConfig &config, std::string &error);
// 已有 JSON 对象时直接解析(例如 server 配置里的 selfAgent 段), 与文件入口同一套规则。
bool jzCliParseConfig(const Json::Value &root, JZCliConfig &config, std::string &error);

// 记忆文件全路径: 统一放在 <rootDir>/.jzagent/memory/ 下面(启动打印与任务共用)。
std::string jzCliMemoryFilePath(const JZCliConfig &config);
// 全局 skills 根: 归一到 <...>/.jzagent(配置给父目录就补 .jzagent)。
std::string jzCliGlobalSkillsDir(const JZCliConfig &config);
// 模型上下文窗口: 只取 llm.models 同名项的 contextWindow(每个模型必填, 无全局默认值)。
int jzCliContextWindow(const JZCliConfig &config, const std::string &modelName);

#endif
