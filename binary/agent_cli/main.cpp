// agent_cli —— 通过 HTTP 暴露任务提交的 agent
//
// 与 agent_node 的区别:
//   agent_node 把本机能力包成 MCP server, 供远端 JZAgent 来调;
//   agent_cli 自己就跑一个 JZAgent(直接驱动本地 agent_tool 工具), 通过 HTTP
//   (POST /task) 对外暴露 —— 外部提交任务 + 验证脚本门控循环。
//
// 配置全部放 config.json(默认 exe 同目录, 可用 -c 指定); 命令行只负责:
//   -c 指定配置文件 / --init 生成默认配置 / --check 校验并打印生效配置 / -h 帮助。
//   --loop "<指令>" = 无人值守模式: 无限循环执行同一条指令(不起 HTTP/网页, 见 main 末尾),
//   配套 --loop-interval <秒>(轮间等待) 与 --verify-cmd "<命令>"(每轮验证)。
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
#include "JZPlatform.h"
#include "JZString.h"
#include "JZDebug.h"

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本, 同步于 binary/agent_cli/config.json)
//   层次化: 顶层只放标识, 其余按 JZCliConfig 的主题归入子对象 —— llm / workspace /
//   tools / runtime / http。子对象内省掉前缀(如 http.bind, 而不是 http.httpBind)。
//   旧配置(全部平铺在顶层, 如 "httpBind")依然可读: 见 loadConfig 里的回落查找。
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
                          "             加 --loop 则改为无人值守: 无限循环执行同一条指令(不启 HTTP/网页)\n");

    string cmdConfig;
    parser.addOption(JZCommandLineOption("--config", "-c",
        "配置文件路径(默认 exe 同目录 config.json)"), &cmdConfig);

    bool cmdInit = false;
    parser.addOption(JZCommandLineOption("--init", "",
        "生成默认 config.json 后退出(不启动)"), &cmdInit);

    bool cmdCheck = false;
    parser.addOption(JZCommandLineOption("--check", "",
        "校验配置并打印生效配置后退出(不启动)"), &cmdCheck);

    // ---- --loop: 无人值守模式(先不管网页) ----
    string cmdLoop;
    parser.addOption(JZCommandLineOption("--loop", "",
        "无人值守: 无限循环执行这条指令(不起 HTTP/网页; 会话跨轮延续, 上下文超预算自动压缩)"),
        &cmdLoop);

    int cmdLoopIntervalSec = 0;
    parser.addOption(JZCommandLineOption("--loop-interval", "",
        "循环模式: 上一轮结束到下一轮开始的等待(秒, 默认 0 = 立刻下一轮)"), &cmdLoopIntervalSec);

    string cmdVerifyCmd;
    parser.addOption(JZCommandLineOption("--verify-cmd", "",
        "循环模式: 每轮的验证命令(退出码 0 通过; 失败会把输出喂回 agent 续跑; 不给 = 不验证)"),
        &cmdVerifyCmd);

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

    JZApplication app;

    JZAgentCli cli;
    if (!cli.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // --loop: 无人值守模式 —— 先不管网页, 连 HTTP / FRP 都不起, 只反复执行这条指令。
    //   每轮都是一次完整任务(带验证命令时含"验证脚本门控循环"), 结果打到控制台(日志)。
    //   没有停止条件: Ctrl+C / 杀进程才停。
    if (!cmdLoop.empty())
    {
        if (cmdVerifyCmd.empty())
            fprintf(stderr, "循环模式: 未给验证命令(--verify-cmd), 每轮只驱动 agent 一次, 不验证\n");
        cli.startLoop(cmdLoop, cmdVerifyCmd, cmdLoopIntervalSec * 1000);
        return app.exec();
    }

    jzDebug("agent_cli 就绪(HTTP): http://%s:%d  token=%s",
            config.httpBind.c_str(), config.httpPort,
            config.httpToken.empty() ? "(无)" : "(已启用)");
    cli.startHttp();
    cli.startFrpc();     // client 角色 = frpc: 连到远端 frps 暴露本地服务(关着就不连)

    return app.exec();
}
