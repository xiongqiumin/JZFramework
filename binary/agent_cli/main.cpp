// agent_cli —— 通过 HTTP 暴露任务提交的 agent
//
// 与 agent_node 的区别:
//   agent_node 把本机能力包成 MCP server, 供远端 JZAgent 来调;
//   agent_cli 自己就跑一个 JZAgent(直接驱动本地 agent_tool 工具), 通过 HTTP
//   (POST /task) 对外暴露 —— 外部提交任务 + 验证脚本门控循环。
//
// 配置全部放 config.json(默认 exe 同目录, 可用 -c 指定); 命令行只负责:
//   -c 指定配置文件 / --init 生成默认配置 / --check 校验并打印生效配置 / -h 帮助。
//
// "多轮"由外部驱动: 谁想反复跑就往 POST /task 反复提交(每轮一次), 跑几轮 / 什么时候停由
// 提交方决定 —— 比如验证程序在自己输出里打 [loop-stop], 提交方看见后不再提交即可。
// 落地见 binary/agent_cli/tools/agent_loop.py(拉起 + 逐轮提交 + 外部复验), 不在 CLI 里另搞
// 一套循环模式。
//
// 日志默认不打印到控制台(避免污染), 可配置 logFile 落盘排查。

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>
#include <string>

#include "JZAgentCli.h"
#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZJson.h"
#include "JZLog.h"
#include "JZNet/JZNetObject.h"    // JZNet::setConfig(抬高 HTTP 接收缓冲, 见 main 里的说明)
#include "JZPlatform.h"
#include "JZString.h"
#include "JZDebug.h"
#include "JZCliConfigIO.h"   // 配置解析(与 inproc 模式共用同一份)

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本, 同步于 binary/agent_cli/config.json)
//   层次化: 顶层只放标识, 其余按 JZCliConfig 的主题归入子对象 —— llm / workspace /
//   tools / runtime / http。子对象内省掉前缀(如 http.bind, 而不是 http.httpBind)。
//   旧配置(全部平铺在顶层, 如 "httpBind")依然可读: 见 JZCliConfigIO.cpp 里的回落查找。
//   mcp.servers[] 一条 = 一个外部 MCP 服务器(见 JZAgentCli.h 的 JZCliMcpServer):
//     stdio: { "name":"fs", "transport":"stdio", "command":"npx",
//              "args":["-y","@modelcontextprotocol/server-filesystem","E:/work"] }
//     http : { "name":"node1", "transport":"http", "url":"http://127.0.0.1:39000/mcp" }
//   可选字段: toolPrefix(工具名前缀; 空 = 对端 serverName + "__") / timeoutMs / callTimeoutMs。
//   远端工具在启动时挂进 agent(任务开跑后再连上的挂不进去, 要重启进程)。
//   system.extra / system.extraFile = 宿主给的**常驻说明**(拼进 system prompt **一次**; 典型是"玩法任务书"
//   这类整个会话都在场的约束) —— 之后每轮 /task 只叠加新消息, 不用把同一段介绍重复贴; extraFile 相对
//   terminal.rootDir 解析。
// ===========================================================================
static const char *kDefaultConfigJson = R"json({
    "agentName":            "agent_cli",
    "version":              "1.0.0",
    "logFile":              "",
    "logRotateDaily":       false,

    "llm": {
        "url":              "",
        "name":             "",
        "token":            "",
        "netTimeoutMs":     120000,
        "maxRounds":        100,
        "maxToken":         160000,
        "maxTokens":        8192,
        "maxRetries":       10,
        "maxFixRetries":    2,
        "retryDelayMs":     1000,
        "thinking":         "auto",
        "body":             {}
    },

    "loop": {
        "enabled":          true,
        "window":           24,
        "repeat":           5,
        "sameResult":       5,
        "pollRepeat":       16,
        "pollSameResult":   6,
        "lineRepeat":       6,
        "blockRepeat":      3,
        "periodMax":        256,
        "periodRepeat":     8
    },

    "verify": {
        "maxRounds":        30,
        "timeoutMs":        60000
    },

    "memory": {
        "enabled":          true,
        "file":             "",
        "maxBytes":         8192
    },

    "system": {
        "extra":            "",
        "extraFile":        ""
    },

    "nodes": [],


    "terminal": {
        "rootDir":          "",
        "trustedDirs":      [],
        "shellProgram":     "",
        "programs":         {},
        "recycleSec":       900,
        "allowTerminal":    true,
        "allowFileTools":   true,
        "profile":          "standard"
    },

    "network": {
        "enabled":          true,
        "searchUrl":        "",
        "proxyHost":        "",
        "proxyPort":        0,
        "proxyUser":        "",
        "proxyPassword":    ""
    },

    "mcp": {
        "enabled":          true,
        "servers":          []
    },

    "server": {
        "host":             "",
        "port":             8090,
        "token":            "",
        "agentId":          ""
    },

    "task": {
        "queues":           1,
        "maxPending":       32
    },

    "http": {
        "bind":             "127.0.0.1",
        "port":             8080,
        "token":            "",
        "dir":              ""
    }
}
)json";

static string pathJoin(const string &dir, const string &name)
{
    if (dir.empty())
        return name;
    char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + name;
    return dir + "/" + name;
}

static bool writeDefaultConfig(const string &path, string &error)
{
    if (JZFileInfo::exists(path))
    {
        error = "配置文件已存在: " + path;
        return false;
    }
    ofstream ofs(path, ios::binary);
    if (!ofs)
    {
        error = "无法写入配置文件: " + path;
        return false;
    }
    ofs << kDefaultConfigJson;
    if (!ofs)
    {
        error = "写入配置文件失败: " + path;
        return false;
    }
    return true;
}


static void printConfig(const string &configPath, const JZCliConfig &config)
{
    string trusted;
    for (size_t i = 0; i < config.trustedDirs.size(); i++)
    {
        if (i)
            trusted += ", ";
        trusted += config.trustedDirs[i].path + (config.trustedDirs[i].writable ? "(rw)" : "(ro)");
    }
    // 打到 stderr: stdout 是 ACP 通道, 不能污染
    fprintf(stderr, "配置文件: %s\n", configPath.c_str());
    fprintf(stderr, "  代理名    %s v%s\n", config.agentName.c_str(), config.version.c_str());
    fprintf(stderr, "  模型      %s @ %s\n",
            config.modelName.empty() ? "(未配置)" : config.modelName.c_str(),
            config.modelUrl.empty() ? "(未配置)" : config.modelUrl.c_str());
    fprintf(stderr, "  思考      %s\n",
            config.thinkMode == ThinkMode::On ? "on(显式发 enable_thinking=true)" :
            config.thinkMode == ThinkMode::Off ? "off(显式发 false)" :
                                                 "auto(不发字段, 端点默认值说了算)");

    // 运行模式: server.host 非空 = managed。两种模式**互斥**, 不是叠加
    if (config.managed())
        fprintf(stderr, "  运行模式  managed(受 server 调度) -> %s:%d  agentId=%s  [不起 HTTP]\n",
                config.server.host.c_str(), config.server.port, config.server.agentId.c_str());
    else
        fprintf(stderr, "  运行模式  standalone(自带 HTTP)\n");

    // 任务队列: 每个队列一个独立 JZAgent
    if (config.task.queues <= 1)
        fprintf(stderr, "  任务队列  1 个(单任务: 忙时按 max_pending 排队或拒绝)\n");
    else
        fprintf(stderr, "  任务队列  %d 个(每队列一个 JZAgent, 队列内串行/队列间并发; 排队上限 %d)\n",
                config.task.queues, config.task.maxPending);

    if (config.extraBody.isObject() && config.extraBody.size() > 0)
    {
        string extra;
        Json::FastWriter writer;
        for (const string &key : config.extraBody.getMemberNames())
        {
            if (!extra.empty())
                extra += ", ";
            string value = writer.write(config.extraBody[key]);
            while (!value.empty() && (value[value.size() - 1] == '\n' || value[value.size() - 1] == '\r'))
                value.erase(value.size() - 1);
            extra += key + "=" + value;
        }
        fprintf(stderr, "  请求体附加 %s\n", extra.c_str());
    }
    string retry = "关";
    if (config.maxRetries > 0 || config.maxFixRetries > 0)
        retry = "瞬时 " + to_string(config.maxRetries) + " 次 + 修正 " + to_string(config.maxFixRetries)
                + " 次, 间隔 " + to_string(config.retryDelayMs) + " ms";
    fprintf(stderr, "  出错重试  %s\n", retry.c_str());
    if (!config.loopDetect)
        fprintf(stderr, "  循环检测  关\n");
    else
        fprintf(stderr, "  循环检测  开 (工具: 窗口 %d 步, 重复 %d 次/结果也相同 %d 次 -> 中止; "
                        "轮询类工具 %d/%d 次; "
                        "输出: 同行 %d 次/同段 %d 次/周期 %d 字符 %d 次 -> 中止)\n",
                config.loopWindow, config.loopRepeat, config.loopSameResult,
                config.loopPollRepeat, config.loopPollSameResult,
                config.loopLineRepeat, config.loopBlockRepeat,
                config.loopPeriodMax, config.loopPeriodRepeat);
    fprintf(stderr, "  验证循环  最多 %d 轮, 单次超时 %d ms\n",
            config.verifyMaxRounds, config.verifyTimeoutMs);
    if (!config.memoryEnabled)
        fprintf(stderr, "  长期记忆  关\n");
    else
        fprintf(stderr, "  长期记忆  %s  (注入上限 %d 字节)\n",
                config.memoryFile.empty()
                    ? (config.rootDir.empty() ? string("<工作区>/.agent_memory.md")
                                              : config.rootDir + "/.agent_memory.md").c_str()
                    : config.memoryFile.c_str(),
                config.memoryMaxBytes);
    fprintf(stderr, "  工作区    %s\n",
            (config.rootDir.empty() ? string(JZDir::currentPath() + "  (配置为空, 取进程目录)") : config.rootDir).c_str());
    fprintf(stderr, "  授信目录  %s\n", trusted.empty() ? "(无)" : trusted.c_str());
    fprintf(stderr, "  工具      终端=%s  文件=%s  网络=%s\n",
            config.allowTerminal ? "开" : "关", config.allowFileTools ? "开" : "关",
            config.allowNetworkTools ? "开" : "关");
    if (config.allowNetworkTools)
    {
        string proxy;
        if (!config.proxyHost.empty())
            proxy = config.proxyHost + ":" + to_string(config.proxyPort);
        fprintf(stderr, "  搜索源    %s\n",
                config.webSearchUrl.empty() ? "内置(Bing RSS)" : config.webSearchUrl.c_str());
        fprintf(stderr, "  网络代理  %s\n", proxy.empty() ? "(不用)" : proxy.c_str());
    }
    // MCP(连外部 MCP 服务器: 远端工具在启动时挂进 agent; 任务开跑之后再连上的挂不进去)
    if (!config.mcpEnabled)
        fprintf(stderr, "  MCP       关\n");
    else if (config.mcpServers.empty())
        fprintf(stderr, "  MCP       开(未配置 server; 在 mcp.servers[] 里加一条即挂远端工具)\n");
    else
    {
        for (const JZCliMcpServer &s : config.mcpServers)
        {
            string target;
            if (s.transport == "http")
                target = s.url;
            else
            {
                target = s.command;
                for (size_t i = 0; i < s.args.size(); i++)
                    target += " " + s.args[i];
            }
            string prefix = s.toolPrefix.empty() ? string() : ("  prefix=" + s.toolPrefix);
            fprintf(stderr, "  MCP       %s (%s) %s%s\n", s.name.c_str(), s.transport.c_str(),
                    target.c_str(), prefix.c_str());
        }
    }
    // 档位展开后的实际策略(单项覆盖也反映在这里)
    {
        JZVirtualTerminalConfig policy = JZVirtualTerminalConfig();
        policy.applyProfile(config.profile);
        if (config.hasAllowPackageManager)
            policy.allowPackageManager = config.allowPackageManager;
        if (config.hasAllowNetwork)
            policy.allowNetwork = config.allowNetwork;
        if (config.hasAllowUnknownPrograms)
            policy.allowUnknownPrograms = config.allowUnknownPrograms;
        fprintf(stderr, "  权限档位  %s(%s)  包管理=%s  联网命令=%s  未知命令=%s\n",
                permissionProfileText(config.profile),
                config.profile == PermissionProfile::Strict ? "严格" :
                config.profile == PermissionProfile::Loose ? "宽松" : "默认",
                policy.allowPackageManager ? "开" : "关",
                policy.allowNetwork ? "开" : "关",
                policy.allowUnknownPrograms ? "放行" : "拒绝");
    }
    // 远端节点(link 模式的 agent_node): 只有配了才会出现 remote_ssh 工具
    if (config.nodes.empty())
        fprintf(stderr, "  远端节点  (无; 纯本地执行)\n");
    else
    {
        for (const JZCliNodeConfig &nc : config.nodes)
        {
            fprintf(stderr, "  远端节点  %s -> %s:%d%s\n", nc.name.c_str(), nc.host.c_str(), nc.port,
                    nc.token.empty() ? "  (无 token)" : "");
        }
    }
    string programs;
    for (const auto &kv : config.programPaths)
    {
        if (!programs.empty())
            programs += ", ";
        programs += kv.first + "=" + kv.second;
    }
    fprintf(stderr, "  shell     %s\n", config.shellProgram.empty()
                                             ? "自动探测(优先 bash, 找不到回退 /bin/sh)"
                                             : config.shellProgram.c_str());
    if (config.logFile.empty())
        fprintf(stderr, "  日志      (不落盘, 只有控制台)\n");
    else
        fprintf(stderr, "  日志      %s/Agent%s.log%s\n",
                jzLogDirOf(config.logFile, JZPlatform::applicationDirPath() + "/data/logs").c_str(),
                config.logRotateDaily ? "-yyyyMMdd" : "",
                config.logRotateDaily ? "  (按天轮转)" : "");
}

int main(int argc, char *argv[])
{
    JZCommandLineParser parser;
    parser.setDescription("agent_cli - HTTP task agent (POST /task)\n"
                          "             反复跑同一件事由提交方驱动: 逐轮 POST /task 即可\n");

    string cmdConfig;
    parser.addOption(JZCommandLineOption("--config", "-c",
        "配置文件路径(默认 exe 同目录 config.json)"), &cmdConfig);

    bool cmdInit = false;
    parser.addOption(JZCommandLineOption("--init", "",
        "生成默认 config.json 后退出(不启动)"), &cmdInit);

    bool cmdCheck = false;
    parser.addOption(JZCommandLineOption("--check", "",
        "校验配置并打印生效配置后退出(不启动)"), &cmdCheck);

    if (!parser.parse(argc, argv))
        return 1;

    string configPath = cmdConfig.empty()
        ? pathJoin(JZPlatform::applicationDirPath(), "config.json")
        : cmdConfig;

    if (cmdInit)
    {
        string err;
        if (!writeDefaultConfig(configPath, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        fprintf(stderr, "已生成默认配置文件: %s\n", configPath.c_str());
        return 0;
    }

    JZCliConfig config;
    string err;
    if (!jzCliLoadConfig(configPath, config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    if (cmdCheck)
    {
        printConfig(configPath, config);
        return 0;
    }

    // net 层"单连接接收缓冲"默认只有 3MB(JZNetConfig::maxRecvSize ⇒ mongoose 的
    // MG_MAX_RECV_SIZE)。/task 带图片(dataURL base64)时 body 会到 MB 级, 超了 mongoose
    // **直接断连** —— 客户端只看到 connection aborted, 连错误回包都没有, 极难排查。
    // 这里抬到 32MB, 与 JZAgentCli 的图片合计上限(24MB)留出 JSON 开销余量。
    {
        JZNetConfig netConfig;
        netConfig.maxRecvSize = 32 * 1024 * 1024;
        JZNet::setConfig(netConfig);
    }

    JZApplication app;

    // 日志: 多模块(JZLog 原生支持, addModule/setModuleConfig 按模块 id 独立配置 type)。
    //  - General: 进程级信息(就绪 / 控制面连接等), 默认开 Console —— agent_cli 的 stdout 被
    //    workflow.py 重定向到 runs/loop/agent_run.out.txt, 不污染真实控制台。
    //  - Agent:   每轮 LLM 回合诊断(think / reply / tool_call / tool_result / input / error),
    //    单独模块、不走 Console; 只有 config.logFile 非空才落盘到 <logFile目录>/Agent.log,
    //    没配就不打印(满足"传入 file 才保存, 没传不打印")。
    {
        JZLogModuleConfig general;
        general.name = "General";
        general.type = JZLogModuleConfig::Console;
        JZLogManager::instance()->setModuleConfig(LogModule_General, general);

        JZLogModuleConfig agent;
        agent.name = "Agent";     // 默认 type=None(不打印); 配了 logFile 才开 File
        JZLogManager::instance()->addModule(LogModule_Agent, agent);
        if (!config.logFile.empty())
        {
            // logFile 一律按**目录**解释(与 server / agent_node 同一口径, 见 jzLogDirOf),
            // 文件名固定 Agent.log; logRotateDaily=true 时按天切成 Agent-yyyyMMdd.log。
            const string logDir = jzLogDirOf(config.logFile, JZPlatform::applicationDirPath() + "/data/logs");
            JZDir().mkpath(logDir);

            JZLogConfig lc;
            lc.shouldPrintTime = true;
            lc.logPath = logDir;
            lc.rotateDaily = config.logRotateDaily;
            JZLogManager::instance()->setConfig(lc);
            agent.type = JZLogModuleConfig::File;
            JZLogManager::instance()->setModuleConfig(LogModule_Agent, agent);
        }
    }

    JZAgentCli cli;
    if (!cli.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // MCP: 尽早发起 —— 远端工具只能在"首个任务之前"挂进 agent(见 JZAgentCli::startMcp),
    // 放在传输层之前, 让连接流程(起子进程 / HTTP 往返)早一点跑完。
    cli.startMcp();

    // 远端节点(link 模式 agent_node): 异步连接, 断线自动重连; 没配就是纯本地执行
    cli.startNodes();

    // 两种模式**互斥**(docs/agent-hub-design.md §3.3):
    //   managed    = 受 server 调度, 任务从控制面来, **不起 HTTP**;
    //   standalone = 自带 HTTP, 提交方直接 POST /task。
    if (config.managed())
    {
        jzDebug("agent_cli 就绪(managed): server=%s:%d  agentId=%s",
                config.server.host.c_str(), config.server.port,
                config.server.agentId.c_str());
        cli.startServerLink();
    }
    else
    {
        jzDebug("agent_cli 就绪(HTTP): http://%s:%d  token=%s",
                config.httpBind.c_str(), config.httpPort,
                config.httpToken.empty() ? "(无)" : "(已启用)");
        cli.startHttp();
    }

    return app.exec();
}
