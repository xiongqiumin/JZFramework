// agent_cli 配置解析 —— 从 main.cpp 抽出来给 inproc(源码引用)模式复用。
#include "JZCliConfigIO.h"

#include <fstream>
#include <sstream>
#include <string>

#include "JZDebug.h"
#include "JZJson.h"
#include "JZString.h"
#include "JZStd.h"

using namespace std;


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

// ---------------------------------------------------------------------------
// 配置读取(严格): 点号路径自动下钻(llm.url / http.bind)。
//   - 缺项 / 缺分组: 保持结构体里的默认值(没配就不动)
//   - 类型不对: **立刻记错并停止**, 由 loadConfig 报出去
//     (一个写错类型的端口号就是配置错误, 不能吞掉继续用默认值启动 —— 那种"看起来
//      正常"的启动最难查)
// ⚠ 只认分组格式; 不回落旧的平铺写法(modelUrl / httpBind)。
// ---------------------------------------------------------------------------
static const Json::Value *findConfigMember(const Json::Value &root, const string &group,
                                          const string &name);

static const char *jsonTypeName(Json::ValueType t)
{
    switch (t)
    {
    case Json::nullValue:   return "null";
    case Json::intValue:    return "整数";
    case Json::uintValue:   return "整数";
    case Json::realValue:   return "小数";
    case Json::stringValue: return "字符串";
    case Json::booleanValue:return "true/false";
    case Json::arrayValue:  return "数组";
    case Json::objectValue: return "对象";
    default:                return "未知";
    }
}

struct CfgReader
{
    explicit CfgReader(const Json::Value &r) : root(r) {}

    bool ok() const { return error.empty(); }

    void str(const std::string &path, std::string &out)
    {
        const Json::Value *v = get(path);
        if (!v)
            return;
        if (!v->isString())
        {
            fail(path, v, "字符串");
            return;
        }
        out = v->asString();
    }

    void i32(const std::string &path, int &out)
    {
        const Json::Value *v = get(path);
        if (!v)
            return;
        if (!v->isInt())
        {
            fail(path, v, "整数");
            return;
        }
        out = v->asInt();
    }

    void flag(const std::string &path, bool &out)
    {
        const Json::Value *v = get(path);
        if (!v)
            return;
        if (!v->isBool())
        {
            fail(path, v, "true/false");
            return;
        }
        out = v->asBool();
    }

    // 配置里到底写没写这一项 —— 档位覆盖要用(写了才覆盖档位预设)
    bool has(const std::string &path) { return get(path) != nullptr; }

    std::string error;   // 第一个错误(空 = 全部通过)

private:
    const Json::Value *get(const std::string &path)
    {
        if (!ok())
            return nullptr;   // 已经错了: 后面的项不再读(报第一个就够)
        return findConfigMember(root, "", path);
    }

    void fail(const std::string &path, const Json::Value *v, const char *want)
    {
        if (!error.empty())
            return;
        error = path + " 应该是" + want + ", 实际是 " + jsonTypeName(v->type());
    }

    const Json::Value &root;
};

// 取一个分组的 Json 值(数组/对象/标量都行), 找不到返回 nullptr。
// 标量一律走 CfgReader(cfg.str/i32/flag); 这里只给"要自己遍历数组/对象"的项用
// (nodes / servers / trustedDirs / programs / body ...)。
static const Json::Value *findConfigMember(const Json::Value &root, const string &group,
                                          const string &name)
{
    const string full = group.empty() ? name : (group + "." + name);
    const Json::Value *cur = &root;
    size_t start = 0;
    while (true)
    {
        const size_t dot = full.find('.', start);
        const std::string seg = (dot == string::npos) ? full.substr(start) : full.substr(start, dot - start);
        if (!cur->isObject() || !cur->isMember(seg))
            return nullptr;
        cur = &(*cur)[seg];
        if (dot == string::npos)
            return cur;
        start = dot + 1;
    }
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
    CfgReader cfg(root);
    cfg.str("agentName", config.agentName);
    cfg.str("version", config.version);

    // llm
    cfg.str("llm.url", config.modelUrl);
    cfg.str("llm.name", config.modelName);
    cfg.str("llm.token", config.modelToken);
    cfg.i32("llm.netTimeoutMs", config.netTimeout);
    cfg.i32("llm.maxRounds", config.maxRounds);
    cfg.i32("llm.maxToken", config.maxToken);
    cfg.i32("llm.maxRetries", config.maxRetries);
    cfg.i32("llm.maxFixRetries", config.maxFixRetries);
    cfg.i32("llm.retryDelayMs", config.retryDelayMs);

    // 思考开关(llm.thinking): "auto"(默认, 不发字段) / "on" / "off"; 也接受 true/false。
    // ⚠ Auto 才是默认, 因为 OpenAI / Azure 这类端点不认 chat_template_kwargs, 发了会 400。
    {
        const Json::Value *think = findConfigMember(root, "llm", "thinking");
        if (think)
        {
            if (think->isBool())
                config.thinkMode = think->asBool() ? ThinkMode::On : ThinkMode::Off;
            else if (think->isString())
            {
                const string t = str_toLower(think->asString());
                if (t == "on" || t == "true" || t == "1")
                    config.thinkMode = ThinkMode::On;
                else if (t == "off" || t == "false" || t == "0")
                    config.thinkMode = ThinkMode::Off;
                else if (t == "auto" || t == "default")
                    config.thinkMode = ThinkMode::Auto;
                else
                {
                    error = "llm.thinking 只能是 auto / on / off(或 true/false): " + think->asString();
                    return false;
                }
            }
            else
            {
                error = "llm.thinking 必须是字符串(auto/on/off)或布尔值";
                return false;
            }
        }
    }

    // 请求体附加字段(llm.body): 原样并进请求体, 用于私有参数 / 采样参数
    // (如 repeat_penalty / dry_multiplier / dry_base / dry_allowed_length、chat_template_kwargs)。
    const Json::Value *body = findConfigMember(root, "llm", "body");
    if (body && body->isObject())
        config.extraBody = *body;
    // 模板里的 llm.maxTokens = OpenAI 输出 token 上限; 经既有 extraBody 通道发送, 不与
    // llm.maxToken(整个上下文预算)混为一谈。llm.body.max_tokens 显式配置优先。
    const Json::Value *maxTokens = findConfigMember(root, "llm", "maxTokens");
    if (maxTokens && maxTokens->isInt() && !config.extraBody.isMember("max_tokens"))
        config.extraBody["max_tokens"] = *maxTokens;

    // loop(循环检测: 工具重复调用 + 思考/回复原地打转 -> 直接中止本轮)
    //   工具侧: 结果也相同 = 证据更强, 阈值更低; 结果不同可能只是合法轮询(等编译/等运行)。
    //   轮询类工具(terminal_list)用 pollRepeat/pollSameResult 单独放宽 ——
    //   反复看"终端在不在跑"是正常用法, 用主阈值会误杀。
    //   输出侧: 同一行刷屏 / 同一段反复 / 周期串复读。
    cfg.flag("loop.enabled", config.loopDetect);
    cfg.i32("loop.window", config.loopWindow);
    cfg.i32("loop.repeat", config.loopRepeat);
    cfg.i32("loop.sameResult", config.loopSameResult);
    cfg.i32("loop.pollRepeat", config.loopPollRepeat);
    cfg.i32("loop.pollSameResult", config.loopPollSameResult);
    cfg.i32("loop.lineRepeat", config.loopLineRepeat);
    cfg.i32("loop.blockRepeat", config.loopBlockRepeat);
    cfg.i32("loop.periodMax", config.loopPeriodMax);
    cfg.i32("loop.periodRepeat", config.loopPeriodRepeat);

    // verify(验证脚本门控循环的默认值: /task 里同名参数可覆盖)
    cfg.i32("verify.maxRounds", config.verifyMaxRounds);
    cfg.i32("verify.timeoutMs", config.verifyTimeoutMs);

    // memory(跨任务长期记忆: 开工注入 + memory 工具读写)
    cfg.flag("memory.enabled", config.memoryEnabled);
    cfg.str("memory.file", config.memoryFile);
    cfg.i32("memory.maxBytes", config.memoryMaxBytes);

    // system(宿主常驻说明: 拼进 system prompt **一次** —— 之后每轮 /task 只叠加新消息, 不再重复贴。
    //   典型是"玩法任务书"这类整个会话都在场的约束; extraFile 相对路径按 rootDir 解析)
    cfg.str("system.extra", config.systemExtra);
    cfg.str("system.extraFile", config.systemExtraFile);

    // project 就是工作区根目录: `project.dir` 只是 `terminal.rootDir` 的别名,
    // 两个都没配 = 进程当前目录(见 JZAgentCli::init)。没有单独的"项目名"。
    cfg.str("project.dir", config.rootDir);
    // 全局 .jzagent 目录(settings 那份): 里面的 skills 总是加载, 空 = %USERPROFILE%/.jzagent
    cfg.str("skillsDir", config.skillsDir);

    // terminal(工作区 / shell / 会话回收 / 工具与命令策略: 都是本地终端这一摊)
    cfg.str("terminal.rootDir", config.rootDir);
    cfg.str("terminal.shellProgram", config.shellProgram);
    cfg.i32("terminal.recycleSec", config.recycleSec);
    cfg.flag("terminal.allowTerminal", config.allowTerminal);
    cfg.flag("terminal.allowFileTools", config.allowFileTools);

    // 权限档位: strict / standard / loose(默认 standard)。见 JZVirtualTerminalConfig.h
    string profileText;
    cfg.str("terminal.profile", profileText);
    if (!profileText.empty() && !permissionProfileFromText(profileText, config.profile))
    {
        error = "terminal.profile 只能是 strict / standard / loose: " + profileText;
        return false;
    }

    // 兼容旧配置(已废弃): 三个单项开关显式写了就覆盖档位预设, 只写档位就别碰
    if (cfg.has("terminal.allowPackageManager"))
    {
        cfg.flag("terminal.allowPackageManager", config.allowPackageManager);
        config.hasAllowPackageManager = true;
    }
    if (cfg.has("terminal.allowNetwork"))
    {
        cfg.flag("terminal.allowNetwork", config.allowNetwork);
        config.hasAllowNetwork = true;
    }
    if (cfg.has("terminal.allowUnknownPrograms"))
    {
        cfg.flag("terminal.allowUnknownPrograms", config.allowUnknownPrograms);
        config.hasAllowUnknownPrograms = true;
    }
    // 兼容旧配置(已废弃): unrestricted: true 等价于宽松档
    if (profileText.empty() && cfg.has("terminal.unrestricted"))
    {
        bool legacyUnrestricted = false;
        cfg.flag("terminal.unrestricted", legacyUnrestricted);
        if (legacyUnrestricted)
            config.profile = PermissionProfile::Loose;
    }

    // 程序映射: 程序名 -> 可执行文件全路径, 如 {"tsc": "D:/app/ts_compiler/tsc/tsc.exe"}
    //   登记后: 短名字进白名单(能过审) + 执行时把短名换成这条全路径(能找到程序)。
    //   只认短名字(写全路径走路径审核); 没登记的程序 agent 就用不了(不做自动探测)。
    const Json::Value *progs = findConfigMember(root, "terminal", "programs");
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

    // network(联网工具: web_search / web_fetch)
    //   与 terminal 档位的 allowNetwork(命令能否用 curl/wget)是**两层**: 这里管 agent 自己的
    //   搜索/抓取工具。默认跟随档位(见下面末尾的推导), 显式写 network.enabled 就以它为准。
    if (cfg.has("network.enabled"))
    {
        cfg.flag("network.enabled", config.allowNetworkTools);
        config.hasNetworkTools = true;
    }
    cfg.str("network.searchUrl", config.webSearchUrl);
    cfg.str("network.proxyHost", config.proxyHost);
    cfg.i32("network.proxyPort", config.proxyPort);
    cfg.str("network.proxyUser", config.proxyUser);
    cfg.str("network.proxyPassword", config.proxyPassword);

    // mcp(连外部 MCP 服务器: 远端工具挂进本 agent; 字段说明见 JZAgentCli.h 的 JZCliMcpServer)
    //   格式: {"enabled":true, "servers":[{"name":"fs","transport":"stdio",
    //          "command":"npx","args":[...]}, {"name":"n1","transport":"http","url":"..."}]}
    cfg.flag("mcp.enabled", config.mcpEnabled);
    const Json::Value *mcpServers = findConfigMember(root, "mcp", "servers");
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

    // nodes(远端执行节点: link 模式的 agent_node)
    //   ⚠ 静态列表: 启动时按它连好, 运行期不增删(连不上就标离线)。
    //   多台 node 在模型侧只体现为**一个** remote_ssh 工具(按 host = 节点名 路由), 见 JZAgentCli::startNodes。
    const Json::Value *nodes = findConfigMember(root, "", "nodes");
    if (nodes && !nodes->isNull())
    {
        if (!nodes->isArray())
        {
            error = "nodes 必须是数组";
            return false;
        }
        for (Json::ArrayIndex i = 0; i < nodes->size(); ++i)
        {
            const Json::Value &v = (*nodes)[i];
            if (!v.isObject())
            {
                error = "nodes[" + to_string(i) + "] 必须是对象";
                return false;
            }
            JZCliNodeConfig nc;
            JZJsonStream ns(&v);
            ns.readMemberDefault("name", nc.name, std::string());
            ns.readMemberDefault("host", nc.host, std::string());
            ns.readMemberDefault("port", nc.port, nc.port);
            ns.readMemberDefault("token", nc.token, std::string());
            if (nc.host.empty())
            {
                error = "nodes[" + to_string(i) + "].host 不能为空";
                return false;
            }
            if (nc.name.empty())
                nc.name = nc.host + ":" + to_string(nc.port);
            if (nc.port <= 0 || nc.port > 65535)
            {
                error = "nodes[" + to_string(i) + "].port 不合法: " + to_string(nc.port);
                return false;
            }
            config.nodes.push_back(nc);
        }
    }

    // 日志(全局项, 不归终端)
    cfg.str("logFile", config.logFile);
    cfg.flag("logRotateDaily", config.logRotateDaily);

    // http
    cfg.str("http.bind", config.httpBind);
    cfg.i32("http.port", config.httpPort);
    cfg.str("http.token", config.httpToken);
    cfg.str("http.dir", config.httpDir);

    // 控制面(managed 模式): server.host 非空即受 server 调度
    //   ⚠ 这是**模式开关**, 不是可选项: managed 时不起 HTTP(见 startServerLink)。
    cfg.str("server.host", config.server.host);
    cfg.i32("server.port", config.server.port);
    cfg.str("server.token", config.server.token);
    cfg.str("server.agentId", config.server.agentId);

    // 任务队列: 每个队列一个独立 JZAgent(独立会话), 队列内串行/队列间并发
    cfg.i32("task.queues", config.task.queues);
    cfg.i32("task.maxPending", config.task.maxPending);
    cfg.i32("task.keepAlive", config.task.keepAlive);
    cfg.i32("task.idleRecycleSec", config.task.idleRecycleSec);
    if (config.task.queues < 1)
        config.task.queues = 1;
    if (config.task.queues > 32)
    {
        jzWarning() << "task.queues 过大(>32), 已按 32 处理";
        config.task.queues = 32;
    }
    if (config.managed())
    {
        if (config.server.port <= 0 || config.server.port > 65535)
        {
            error = "server.port 不合法: " + to_string(config.server.port);
            return false;
        }
        if (config.server.agentId.empty())
            config.server.agentId = config.agentName;
    }

    const Json::Value *dirs = findConfigMember(root, "terminal", "trustedDirs");
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

    // 标量项的类型错误在这里统一报出去(读的时候只记第一个, 后面不再读)
    if (!cfg.ok())
    {
        error = "配置项有误: " + cfg.error;
        return false;
    }

    // 联网工具跟随权限档位: 严格档禁联网 → 不注册 web_search/web_fetch;
    // 默认/宽松档允许联网 → 注册。配置里显式写了 network.enabled 就不动。
    if (!config.hasNetworkTools)
        config.allowNetworkTools = (config.profile != PermissionProfile::Strict);

    return true;
}

bool jzCliLoadConfig(const std::string &path, JZCliConfig &config, std::string &error)
{
    return loadConfig(path, config, error);
}
