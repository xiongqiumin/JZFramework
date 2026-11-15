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

#endif
