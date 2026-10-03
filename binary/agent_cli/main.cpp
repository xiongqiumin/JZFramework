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

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本, 同步于 binary/agent_cli/config.json)
//   层次化: 顶层只放标识, 其余按 JZCliConfig 的主题归入子对象 —— llm / workspace /
//   tools / runtime / http。子对象内省掉前缀(如 http.bind, 而不是 http.httpBind)。
//   旧配置(全部平铺在顶层, 如 "httpBind")依然可读: 见 loadConfig 里的回落查找。
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
        "body":             {}
    },

    "loop": {
        "enabled":          true,
        "window":           24,
        "repeat":           5,
        "sameResult":       2,
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

    "terminal": {
        "rootDir":          "",
        "trustedDirs":      [],
        "shellProgram":     "",
        "programs":         {},
        "recycleSec":       900,
        "allowTerminal":    true,
        "allowFileTools":   true,
        "allowPackageManager": false,
        "allowNetwork":     false,
        "unrestricted":     false
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

    "http": {
        "bind":             "127.0.0.1",
        "port":             8080,
        "token":            "",
        "dir":              ""
    },

    "frp": {
        "server": {
            "enabled":      false,
            "bindPort":     7000,
            "token":        ""
        },
        "client": {
            "enabled":      false,
            "serverAddr":   "",
            "serverPort":   7000,
            "token":        "",
            "proxies": [
                { "name": "http", "localIp": "127.0.0.1", "localPort": 8080, "remotePort": 8080 }
            ]
        }
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

// "D:/sdk:rw" -> {path:"D:/sdk", writable:true}(盘符冒号不能当分隔符, 只看末尾 ro/rw)
static bool parseTrusted(const string &text, TrustedDir &out)
{
    out.writable = false;
    out.path = text;
    size_t pos = text.rfind(':');
    if (pos != string::npos && pos + 1 < text.size())
    {
        string suffix = str_toLower(text.substr(pos + 1));
        if (suffix == "ro" || suffix == "rw")
        {
            out.writable = (suffix == "rw");
            out.path = text.substr(0, pos);
        }
    }
    return !out.path.empty();
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

// ---------------------------------------------------------------------------
// 分层配置的取值: 优先 "<group>.<name>"(现格式), 回落顶层 "<legacy>"(旧平铺格式)
//   例: http.bind / httpBind,  llm.url / modelUrl。legacy 为空表示没有旧写法。
//   命中才写回 out, 所以缺省值沿用 JZCliConfig 里的默认。
// ---------------------------------------------------------------------------
static const Json::Value *findConfigMember(const Json::Value &root, const string &group,
                                          const string &name, const string &legacy)
{
    // 优先走分组: name 支持 "a.b.c" 多级嵌套(如 frp.client.enabled ->
    //   root["frp"]["client"]["enabled"]); 单段名(如 http.bind)同样适用。
    if (!group.empty() && root.isMember(group) && root[group].isObject())
    {
        const Json::Value *cur = &root[group];
        size_t start = 0;
        while (true)
        {
            size_t dot = name.find('.', start);
            std::string seg = (dot == std::string::npos)
                ? name.substr(start) : name.substr(start, dot - start);
            if (!cur->isObject() || !cur->isMember(seg))
                break;
            cur = &(*cur)[seg];
            if (dot == std::string::npos)
                return cur;
            start = dot + 1;
        }
    }

    if (!legacy.empty() && root.isMember(legacy))
        return &root[legacy];

    return nullptr;
}

static void readConfigString(const Json::Value &root, const string &group, const string &name,
                             const string &legacy, string &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isString())
        out = v->asString();
}

static void readConfigInt(const Json::Value &root, const string &group, const string &name,
                          const string &legacy, int &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isNumeric())
        out = v->asInt();
}

static void readConfigBool(const Json::Value &root, const string &group, const string &name,
                           const string &legacy, bool &out)
{
    const Json::Value *v = findConfigMember(root, group, name, legacy);
    if (v && v->isBool())
        out = v->asBool();
}

static bool loadConfig(const string &path, JZCliConfig &config, string &error)
{
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        error = "配置文件不存在或无法打开: " + path;
        return false;
    }
    stringstream ss;
    ss << ifs.rdbuf();
    string text = ss.str();

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(text, root))
    {
        error = "配置文件 JSON 解析失败: " + reader.getFormattedErrorMessages();
        return false;
    }
    if (!root.isObject())
    {
        error = "配置文件根必须是 JSON 对象";
        return false;
    }

    // 标识(顶层)
    JZJsonStream js(&root);
    js.readMemberDefault("agentName", config.agentName, config.agentName);
    js.readMemberDefault("version", config.version, config.version);

    // llm
    readConfigString(root, "llm", "url", "modelUrl", config.modelUrl);
    readConfigString(root, "llm", "name", "modelName", config.modelName);
    readConfigString(root, "llm", "token", "modelToken", config.modelToken);
    readConfigInt(root, "llm", "netTimeoutMs", "netTimeout", config.netTimeout);
    readConfigInt(root, "llm", "maxRounds", "maxRounds", config.maxRounds);
    readConfigInt(root, "llm", "maxToken", "maxToken", config.maxToken);
    readConfigInt(root, "llm", "maxRetries", "maxRetries", config.maxRetries);
    readConfigInt(root, "llm", "maxFixRetries", "maxFixRetries", config.maxFixRetries);
    readConfigInt(root, "llm", "retryDelayMs", "retryDelayMs", config.retryDelayMs);

    // 请求体附加字段(llm.body): 原样并进请求体, 用于私有参数 / 采样参数
    // (如 repeat_penalty / dry_multiplier / dry_base / dry_allowed_length、chat_template_kwargs)。
    const Json::Value *body = findConfigMember(root, "llm", "body", "extraBody");
    if (body && body->isObject())
        config.extraBody = *body;

    // loop(循环检测: 工具重复调用 + 思考/回复原地打转 -> 直接中止本轮)
    //   工具侧: 结果也相同 = 证据更强, 阈值更低; 结果不同可能只是合法轮询(等编译/等运行)。
    //   轮询类工具(terminal_query/terminal_list)用 pollRepeat/pollSameResult 单独放宽 ——
    //   提示词本身就教模型"用 terminal_query 反复查进度", 用主阈值会误杀。
    //   输出侧: 同一行刷屏 / 同一段反复 / 周期串复读。
    readConfigBool(root, "loop", "enabled", "loopDetect", config.loopDetect);
    readConfigInt(root, "loop", "window", "loopWindow", config.loopWindow);
    readConfigInt(root, "loop", "repeat", "loopRepeat", config.loopRepeat);
    readConfigInt(root, "loop", "sameResult", "loopSameResult", config.loopSameResult);
    readConfigInt(root, "loop", "pollRepeat", "loopPollRepeat", config.loopPollRepeat);
    readConfigInt(root, "loop", "pollSameResult", "loopPollSameResult", config.loopPollSameResult);
    readConfigInt(root, "loop", "lineRepeat", "loopLineRepeat", config.loopLineRepeat);
    readConfigInt(root, "loop", "blockRepeat", "loopBlockRepeat", config.loopBlockRepeat);
    readConfigInt(root, "loop", "periodMax", "loopPeriodMax", config.loopPeriodMax);
    readConfigInt(root, "loop", "periodRepeat", "loopPeriodRepeat", config.loopPeriodRepeat);

    // verify(验证脚本门控循环的默认值: /task 里同名参数可覆盖)
    readConfigInt(root, "verify", "maxRounds", "verifyMaxRounds", config.verifyMaxRounds);
    readConfigInt(root, "verify", "timeoutMs", "verifyTimeoutMs", config.verifyTimeoutMs);

    // memory(跨任务长期记忆: 开工注入 + memory 工具读写)
    readConfigBool(root, "memory", "enabled", "memoryEnabled", config.memoryEnabled);
    readConfigString(root, "memory", "file", "memoryFile", config.memoryFile);
    readConfigInt(root, "memory", "maxBytes", "memoryMaxBytes", config.memoryMaxBytes);

    // system(宿主常驻说明: 拼进 system prompt **一次** —— 之后每轮 /task 只叠加新消息, 不再重复贴。
    //   典型是"玩法任务书"这类整个会话都在场的约束; extraFile 相对路径按 rootDir 解析)
    readConfigString(root, "system", "extra", "systemExtra", config.systemExtra);
    readConfigString(root, "system", "extraFile", "systemExtraFile", config.systemExtraFile);

    // terminal(工作区 / shell / 会话回收 / 工具与命令策略: 都是本地终端这一摊)
    readConfigString(root, "terminal", "rootDir", "rootDir", config.rootDir);
    readConfigString(root, "terminal", "shellProgram", "shellProgram", config.shellProgram);
    readConfigInt(root, "terminal", "recycleSec", "recycleSec", config.recycleSec);
    readConfigBool(root, "terminal", "allowTerminal", "allowTerminal", config.allowTerminal);
    readConfigBool(root, "terminal", "allowFileTools", "allowFileTools", config.allowFileTools);
    readConfigBool(root, "terminal", "allowPackageManager", "allowPackageManager", config.allowPackageManager);
    readConfigBool(root, "terminal", "allowNetwork", "allowNetwork", config.allowNetwork);
    readConfigBool(root, "terminal", "unrestricted", "unrestricted", config.unrestricted);

    // 程序映射: 程序名 -> 可执行文件全路径, 如 {"tsc": "D:/app/ts_compiler/tsc/tsc.exe"}
    //   登记后: 短名字进白名单(能过审) + 执行时把短名换成这条全路径(能找到程序)。
    //   只认短名字(写全路径走路径审核); 没登记的程序 agent 就用不了(不做自动探测)。
    const Json::Value *progs = findConfigMember(root, "terminal", "programs", "programs");
    if (progs && progs->isObject())
    {
        for (Json::Value::const_iterator it = progs->begin(); it != progs->end(); ++it)
        {
            string name = str_toLower(it.key().asString());
            string path = (*it).isString() ? (*it).asString() : string();
            if (!name.empty() && !path.empty())
                config.programPaths[name] = path;
        }
    }

    // network(联网工具: web_search / web_fetch —— 与 terminal.allowNetwork(命令能否联网)独立)
    readConfigBool(root, "network", "enabled", "allowNetworkTools", config.allowNetworkTools);
    readConfigString(root, "network", "searchUrl", "webSearchUrl", config.webSearchUrl);
    readConfigString(root, "network", "proxyHost", "proxyHost", config.proxyHost);
    readConfigInt(root, "network", "proxyPort", "proxyPort", config.proxyPort);
    readConfigString(root, "network", "proxyUser", "proxyUser", config.proxyUser);
    readConfigString(root, "network", "proxyPassword", "proxyPassword", config.proxyPassword);

    // mcp(连外部 MCP 服务器: 远端工具挂进本 agent; 字段说明见 JZAgentCli.h 的 JZCliMcpServer)
    //   格式: {"enabled":true, "servers":[{"name":"fs","transport":"stdio",
    //          "command":"npx","args":[...]}, {"name":"n1","transport":"http","url":"..."}]}
    readConfigBool(root, "mcp", "enabled", "mcpEnabled", config.mcpEnabled);
    const Json::Value *mcpServers = findConfigMember(root, "mcp", "servers", "mcpServers");
    if (mcpServers && !mcpServers->isNull())
    {
        if (!mcpServers->isArray())
        {
            error = "mcp.servers 必须是数组";
            return false;
        }
        for (Json::ArrayIndex i = 0; i < mcpServers->size(); ++i)
        {
            const Json::Value &v = (*mcpServers)[i];
            if (!v.isObject())
            {
                error = "mcp.servers[" + to_string(i) + "] 必须是对象";
                return false;
            }
            JZCliMcpServer server;
            JZJsonStream ms(&v);
            ms.readMemberDefault("name", server.name, std::string());
            ms.readMemberDefault("transport", server.transport, std::string("stdio"));
            ms.readMemberDefault("command", server.command, std::string());
            ms.readMemberDefault("url", server.url, std::string());
            ms.readMemberDefault("toolPrefix", server.toolPrefix, std::string());
            ms.readMemberDefault("timeoutMs", server.timeoutMs, 10000);
            ms.readMemberDefault("callTimeoutMs", server.callTimeoutMs, 120000);

            if (v.isMember("args"))
            {
                if (!v["args"].isArray())
                {
                    error = "mcp.servers[" + to_string(i) + "].args 必须是数组";
                    return false;
                }
                for (Json::ArrayIndex k = 0; k < v["args"].size(); ++k)
                    server.args.push_back(v["args"][k].asString());
            }

            server.transport = str_toLower(server.transport);
            if (server.name.empty())
                server.name = server.transport;
            if (server.transport != "stdio" && server.transport != "http")
            {
                error = "mcp.servers[" + to_string(i) + "].transport 必须是 stdio 或 http";
                return false;
            }
            if (server.transport == "stdio" && server.command.empty())
            {
                error = "mcp.servers[" + to_string(i) + "](stdio) 缺 command";
                return false;
            }
            if (server.transport == "http" && server.url.empty())
            {
                error = "mcp.servers[" + to_string(i) + "](http) 缺 url";
                return false;
            }
            config.mcpServers.push_back(server);
        }
    }

    // 日志(全局项, 不归终端)
    readConfigString(root, "", "logFile", "logFile", config.logFile);

    // http
    readConfigString(root, "http", "bind", "httpBind", config.httpBind);
    readConfigInt(root, "http", "port", "httpPort", config.httpPort);
    readConfigString(root, "http", "token", "httpToken", config.httpToken);
    readConfigString(root, "http", "dir", "httpDir", config.httpDir);

    // frp(client 角色 = frpc; 服务端 frps 在 JZAgentCliServer)
    //   本端连到远端 frps, 把本地服务经 proxy 暴露出去。
    readConfigBool(root, "frp", "client.enabled", "frpClientEnabled", config.frp.client.enabled);
    readConfigString(root, "frp", "client.serverAddr", "frpServerAddr", config.frp.client.serverAddr);
    readConfigInt(root, "frp", "client.serverPort", "frpServerPort", config.frp.client.serverPort);
    readConfigString(root, "frp", "client.token", "frpClientToken", config.frp.client.token);
    const Json::Value *frpProxies = findConfigMember(root, "frp", "client.proxies", "frpProxies");
    if (frpProxies && frpProxies->isArray())
    {
        for (Json::ArrayIndex i = 0; i < frpProxies->size(); ++i)
        {
            const Json::Value &p = (*frpProxies)[i];
            if (!p.isObject())
                continue;
            JZCliFrpProxy proxy;
            JZJsonStream ps(&p);
            ps.readMemberDefault("name", proxy.name, std::string());
            ps.readMemberDefault("localIp", proxy.localIp, std::string("127.0.0.1"));
            ps.readMemberDefault("localPort", proxy.localPort, 0);
            ps.readMemberDefault("remotePort", proxy.remotePort, 0);
            // localIp 允许空(默认 127.0.0.1); 但端口必须有效才收
            if (proxy.localPort > 0 && proxy.remotePort > 0)
                config.frp.client.proxies.push_back(proxy);
        }
    }

    const Json::Value *dirs = findConfigMember(root, "terminal", "trustedDirs", "trustedDirs");
    const Json::Value &arr = dirs ? *dirs : Json::Value::nullRef;
    if (!arr.isNull())
    {
        if (!arr.isArray())
        {
            error = "trustedDirs 必须是数组";
            return false;
        }
        for (Json::ArrayIndex i = 0; i < arr.size(); ++i)
        {
            const Json::Value &v = arr[i];
            TrustedDir dir;
            if (v.isString())
            {
                if (!parseTrusted(v.asString(), dir))
                {
                    error = "trustedDirs[" + to_string(i) + "] 目录为空";
                    return false;
                }
                config.trustedDirs.push_back(dir);
            }
            else if (v.isObject())
            {
                JZJsonStream ds(&v);
                if (!ds.readMember("path", dir.path) || dir.path.empty())
                {
                    error = "trustedDirs[" + to_string(i) + "].path 缺失或为空";
                    return false;
                }
                ds.readMemberDefault("writable", dir.writable, false);
                config.trustedDirs.push_back(dir);
            }
            else
            {
                error = "trustedDirs[" + to_string(i) + "] 必须是字符串或对象";
                return false;
            }
        }
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
    fprintf(stderr, "  命令策略  包管理=%s  联网命令=%s  权限=%s\n",
            config.allowPackageManager ? "开" : "关",
            config.allowNetwork ? "开" : "关",
            config.unrestricted ? "全部不检查" : "按区域分档");
    // frp(client 角色 = frpc; 服务端 frps 在 JZAgentCliServer)
    if (!config.frp.client.enabled)
        fprintf(stderr, "  FRP       关(仅 client 角色; 服务端在 JZAgentCliServer)\n");
    else
        fprintf(stderr, "  FRP 客户端  frpc -> %s:%d, 代理 %d 条\n",
                config.frp.client.serverAddr.empty() ? "(未配 serverAddr)"
                                                     : config.frp.client.serverAddr.c_str(),
                config.frp.client.serverPort, (int)config.frp.client.proxies.size());
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
    fprintf(stderr, "  日志文件  %s\n", config.logFile.empty() ? "(不打印到控制台)" : config.logFile.c_str());
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
    if (!loadConfig(configPath, config, err))
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
    //  - General: 进程级信息(就绪 / frps 连接等), 默认开 Console —— agent_cli 的 stdout 被
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
            JZLogConfig lc;
            lc.shouldPrintTime = true;
            lc.logPath = ([&]() {
                auto p = config.logFile.find_last_of("/\\");
                return p == std::string::npos ? std::string(".") : config.logFile.substr(0, p);
            })();
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

    // 就绪: 常驻等 POST /task(每提交一次 = 一轮; 会话跨轮延续, 上下文超预算自动压缩)。
    //   "反复跑同一件事"由提交方驱动, 见文件头说明与 tools/agent_loop.py。
    jzDebug("agent_cli 就绪(HTTP): http://%s:%d  token=%s",
            config.httpBind.c_str(), config.httpPort,
            config.httpToken.empty() ? "(无)" : "(已启用)");
    // MCP: 尽早发起 —— 远端工具只能在"首个任务之前"挂进 agent(见 JZAgentCli::startMcp),
    // 放 startHttp 前面, 让连接流程(起子进程 / HTTP 往返)早一点跑完。
    cli.startMcp();

    cli.startHttp();
    cli.startFrpc();     // client 角色 = frpc: 连到远端 frps 暴露本地服务(关着就不连)

    return app.exec();
}
