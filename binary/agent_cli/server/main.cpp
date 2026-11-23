// JZAgentCliServer —— agent_cli 管理服务器
//
//   1) 控制面(hub): 等远端 cli 主动连进来, 点名下发 A2A Task;
//   2) 管理一系列 JZAgentCli 实例: 每个作为一个子进程拉起并监管(启停/重启/状态);
//   3) 收集各 agent 的 stdout/stderr 日志, 提供后台网页 + 管理 API 查询。
//
// 配置放在 config.json(默认 exe 同目录, 可用 -c 指定); 命令行只负责:
//   -c 指定配置文件 / --init 生成默认配置 / --check 校验并打印生效配置 / -h 帮助。

#include <stdio.h>
#include <stdlib.h>

#include <csignal>
#include <fstream>
#include <sstream>
#include <string>

#include "JZAgentCliServer.h"
#include "JZApplication.h"
#include "JZCliConfigIO.h"   // 内置 agent 与 agent_cli 共用配置解析
#include "JZCommandLineParser.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZJson.h"
#include "JZLog.h"
#include "JZPlatform.h"
#include "JZString.h"
#include "JZTimer.h"
#include "JZDebug.h"

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本)。
//   http.*     后台网页 / 管理 API 的监听地址。
//   http.trustProxy  是否信任反向代理写的 X-Forwarded-For(默认 false = 只认 TCP 对端)。
//              挡在 nginx 后面且端口只对反代可达时才开; 否则客户端能自带这个头伪装 IP,
//              按 IP 的失败封禁(http.auth.*)就形同虚设。
//   http.ssl.* HTTPS 默认开启(单向 TLS, 只加密不认证客户端); 证书路径相对配置文件目录。
//              要跑起来必须先备好 certs/{server.crt,server.key}; 缺文件时 server 直接启动失败,
//              不会退回明文。客户端证书(mTLS)已于 2026-11-14 移除, 应用层鉴权改为
//              http.token: 开了 TLS 就**必须**配(见 loadConfig 里的校验), 校验范围 /api/*。
//   agents[]   被管理的一系列 agent_cli(各作为一个子进程拉起并监管)。
//   内网穿透不在本进程内: 需要时用独立的 jzfrps / jzfrpc 进程。
// ===========================================================================
static const char *kDefaultConfigJson = R"json({
    "logFile":        "",
    "logRotateDaily": false,
    "http": {
        "bind":   "127.0.0.1",
        "port":   8088,
        "token":  "",
        "trustProxy": false,
        "auth": { "maxFails": 5, "banSeconds": 300 },
        "ssl": {
            "enabled":  true,
            "certFile": "certs/server.crt",
            "keyFile":  "certs/server.key"
        }
    },
    "hub": {
        "bind":  "0.0.0.0",
        "port":  8090,
        "token": ""
    },
    "agents": [
        {
            "name":    "agent-1",
            "enabled": true,
            "args":    ["-c", "agent1.json"],
            "dir":     ""
        }
    ]
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

// 配置文件所在目录(用于把证书相对路径解析成绝对路径)
static string dirName(const string &path)
{
    size_t p = path.find_last_of("/\\");
    return (p == string::npos) ? string() : path.substr(0, p);
}

// 相对路径按 baseDir 展开; 已经是绝对路径(盘符 / 以 / 或 \ 开头)的原样返回
static string resolvePath(const string &baseDir, const string &path)
{
    if (path.empty())
        return path;
    bool isAbs = path[0] == '/' || path[0] == '\\' ||
                 (path.size() > 1 && path[1] == ':');
    if (isAbs)
        return path;
    return pathJoin(baseDir, path);
}

static string joinStrings(const vector<string> &v, const string &sep)
{
    string out;
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (i) out += sep;
        out += v[i];
    }
    return out;
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
// 分层配置取值: 优先 "<group>.<name>", 回落顶层 legacy("<legacy>")。
// ---------------------------------------------------------------------------
static const Json::Value *findMember(const Json::Value &root, const string &group,
                                     const string &name, const string &legacy)
{
    // name 支持 "a.b.c" 多级嵌套(如 http.ssl.enabled -> root["http"]["ssl"]["enabled"])
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

static void readBool(const Json::Value &root, const string &group, const string &name,
                     const string &legacy, bool &out)
{
    const Json::Value *v = findMember(root, group, name, legacy);
    if (v && v->isBool())
        out = v->asBool();
}

static void readInt(const Json::Value &root, const string &group, const string &name,
                    const string &legacy, int &out)
{
    const Json::Value *v = findMember(root, group, name, legacy);
    if (v && v->isNumeric())
        out = v->asInt();
}

static void readString(const Json::Value &root, const string &group, const string &name,
                       const string &legacy, string &out)
{
    const Json::Value *v = findMember(root, group, name, legacy);
    if (v && v->isString())
        out = v->asString();
}

static bool loadConfig(const string &path, JZCliServerConfig &config, string &error)
{
    ifstream ifs(path, ios::binary);
    if (!ifs)
    {
        error = "配置文件不存在或无法打开: " + path;
        return false;
    }
    stringstream ss;
    ss << ifs.rdbuf();

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(ss.str(), root))
    {
        error = "配置文件 JSON 解析失败: " + reader.getFormattedErrorMessages();
        return false;
    }
    if (!root.isObject())
    {
        error = "配置文件根必须是 JSON 对象";
        return false;
    }

    // http: 后台网页 / 管理 API
    readString(root, "http", "bind", "httpBind", config.http.bind);
    readInt(root, "http", "port", "httpPort", config.http.port);

    // http.ssl: HTTPS(默认开, 单向 TLS)。证书路径相对配置文件目录。
    //   ssl.caFile / ssl.requireClientCert 是旧的双向认证字段, 已于 2026-11-14 移除;
    //   老配置里留着它们也不报错, 只是不再生效(应用层鉴权用 http.token)。
    readBool(root, "http", "ssl.enabled", "httpSslEnabled", config.http.sslEnabled);
    // 反向代理后面才打开: 客户端 IP 取 X-Forwarded-For 而不是 TCP 对端。
    // 默认关 —— 开着而端口又直接暴露在外 = 谁都能伪造 IP, 按 IP 的失败封禁失效。
    readBool(root, "http", "trustProxy", "httpTrustProxy", config.http.trustProxy);
    readString(root, "http", "token", "httpToken", config.http.token);
    readInt(root, "http", "auth.maxFails", "httpAuthMaxFails", config.http.authMaxFails);
    readInt(root, "http", "auth.banSeconds", "httpAuthBanSec", config.http.authBanSeconds);
    if (config.http.authMaxFails < 1)
        config.http.authMaxFails = 1;      // 0/负数会让"永远不封"变成"一次就封"的错觉
    if (config.http.authBanSeconds < 0)
        config.http.authBanSeconds = 0;
    readString(root, "http", "ssl.certFile", "httpSslCert", config.http.certFile);
    readString(root, "http", "ssl.keyFile", "httpSslKey", config.http.keyFile);

    // HTTPS 只保证"路上没人看", 不保证"对面是谁" —— 端口一旦从不可信网络可达, 谁都能
    // 起停 agent / 下发任务。所以开 TLS 就必须带应用层 token; 想裸奔请显式关掉 ssl。
    if (config.http.sslEnabled && config.http.token.empty())
    {
        error = "http.ssl.enabled=true 时必须配置 http.token(应用层鉴权); "
                "只用 TLS 不认证调用方。要么填 http.token, 要么显式设 http.ssl.enabled=false";
        return false;
    }

    if (config.http.certFile.empty())
        config.http.certFile = "certs/server.crt";
    if (config.http.keyFile.empty())
        config.http.keyFile = "certs/server.key";

    const string cfgDir = dirName(path);

    // logFile: 空 = <exeDir>/data/logs; 相对路径按"配置文件所在目录"解析
    if (root.isMember("logFile") && root["logFile"].isString() && !root["logFile"].asString().empty())
        config.logFile = resolvePath(cfgDir, root["logFile"].asString());
    readBool(root, "", "logRotateDaily", "logRotateDaily", config.logRotateDaily);

    config.http.certFile = resolvePath(cfgDir, config.http.certFile);
    config.http.keyFile = resolvePath(cfgDir, config.http.keyFile);

    // hub: 控制面(JZNet), agent_cli 出站连这里 —— 这才是"管远端 cli"的通道,
    //      http.* 只是给人看的网页/API, 两者是两个端口。
    readString(root, "hub", "bind", "hubBind", config.hub.bind);
    readInt(root, "hub", "port", "hubPort", config.hub.port);
    readString(root, "hub", "token", "hubToken", config.hub.token);
    if (config.hub.port <= 0 || config.hub.port > 65535 || config.hub.port == config.http.port)
    {
        error = "hub.port 不合法(不能等于 http.port): " + to_string(config.hub.port);
        return false;
    }

    // agents[]: 被管理的一系列 agent_cli
    const Json::Value *agents = findMember(root, "", "agents", "agents");
    if (agents && agents->isArray())
    {
        for (Json::ArrayIndex i = 0; i < agents->size(); ++i)
        {
            const Json::Value &a = (*agents)[i];
            if (!a.isObject())
                continue;
            JZCliServerAgentConfig ac;
            JZJsonStream as(&a);
            as.readMemberDefault("name", ac.name, string());
            as.readMemberDefault("enabled", ac.enabled, true);
            as.readMemberDefault("dir", ac.dir, string());
            // 兼容旧配置: cmd / mode 已取消(cli 一律源码引用跑在本进程的线程里), 读到也不用
            const Json::Value *args = a.isMember("args") ? &a["args"] : nullptr;
            if (args && args->isArray())
                for (Json::ArrayIndex k = 0; k < args->size(); ++k)
                    if ((*args)[k].isString())
                        ac.args.push_back((*args)[k].asString());
            if (!ac.name.empty())
                config.agents.push_back(ac);
        }
    }

    // selfAgent: server 进程自己跑的那个 JZAgent(内置 agent, agentId 默认 "server")
    //   与远端 cli 平级出现在 agent 列表里; 它多了几个"控制面"工具(见 JZServerAgent.cpp)。
    //   运行参数与 agent_cli 共用 JZCliConfigIO 解析: 老配置里的平铺字段在这里映射成分组格式。
    const Json::Value *self = findMember(root, "", "selfAgent", "selfAgent");
    if (self && self->isObject())
    {
        JZJsonStream as(self);
        as.readMemberDefault("enabled", config.selfAgentEnabled, false);
        as.readMemberDefault("agentId", config.selfAgentId, string("server"));
        as.readMemberDefault("name", config.selfAgentName, string("server(内置)"));

        // 保留内置 agent 原来的缺省值(与独立 cli 的 JZCliConfig 默认值不同)
        config.selfAgent.model.netTimeout = 120000;
        config.selfAgent.maxRounds = 40;

        Json::Value cliRoot(Json::objectValue);
        if (self->isMember("models"))      cliRoot["llm"]["models"] = (*self)["models"];
        if (self->isMember("netTimeout"))  cliRoot["llm"]["netTimeoutMs"] = (*self)["netTimeout"];
        if (self->isMember("maxRounds"))   cliRoot["llm"]["maxRounds"] = (*self)["maxRounds"];
        if (self->isMember("rootDir"))     cliRoot["terminal"]["rootDir"] = (*self)["rootDir"];
        if (self->isMember("systemExtra")) cliRoot["system"]["extra"] = (*self)["systemExtra"];
        if (self->isMember("skillsDir"))   cliRoot["skillsDir"] = (*self)["skillsDir"];

        string cliError;
        if (!jzCliParseConfig(cliRoot, config.selfAgent, cliError))
        {
            error = "selfAgent 配置错误: " + cliError;
            return false;
        }
        if (config.selfAgentEnabled && config.selfAgentId.empty())
            config.selfAgentId = "server";
        config.selfAgent.agentName = config.selfAgentId;

        if (config.selfAgentEnabled && config.selfAgent.model.models.empty())
        {
            error = "selfAgent.enabled=true 但没配 selfAgent.models[]";
            return false;
        }
    }
    return true;
}

static void printConfig(const string &configPath, const JZCliServerConfig &config)
{
    // 打到 stderr: stdout 留给将来扩展(如 ACP 通道)
    fprintf(stderr, "配置文件: %s\n", configPath.c_str());
    fprintf(stderr, "  角色      agent_cli 管理服务器(控制面 + 后台网页 + agent 监管)\n");

    // http 网页 / API(HTTPS 只加密; /api/* 由 http.token 鉴权)
    fprintf(stderr, "  网页      %s://%s:%d\n",
            config.http.sslEnabled ? "https" : "http", config.http.bind.c_str(),
            config.http.port);
    if (config.http.sslEnabled)
    {
        fprintf(stderr, "    TLS     证书=%s[%s]  私钥=%s[%s]  单向(不认证客户端)\n",
                config.http.certFile.c_str(), JZFileInfo::exists(config.http.certFile) ? "在" : "缺失",
                config.http.keyFile.c_str(), JZFileInfo::exists(config.http.keyFile) ? "在" : "缺失");
    }
    else
        fprintf(stderr, "    ⚠ HTTPS 已关闭: 明文 HTTP(仅限完全可信的隔离网络)\n");
    {
        // ⚠ 先拼成 std::string 再 .c_str(): 三元表达式两支类型不同(const char* / string)
        //   会统一成 std::string, 直接把 string 交给 %s 就是未定义行为(实测打出乱码)。
        const string auth = config.http.token.empty()
            ? string("无 token(仅限完全可信的隔离网络; 且只绑 127.0.0.1)")
            : ("token(校验 /api/* 与 /app.js, 来源: cookie jz_token / X-JZ-Token / Bearer; "
               "5 分钟内错 " + std::to_string(config.http.authMaxFails) + " 次封 " +
               std::to_string(config.http.authBanSeconds) + " 秒)");
        fprintf(stderr, "    鉴权     %s\n", auth.c_str());
    }
    // 客户端 IP 从哪来: 决定"按 IP 的失败封禁"是否可信
    fprintf(stderr, "    客户端IP %s\n", config.http.trustProxy
            ? "取自 X-Forwarded-For(确认端口只对反向代理可达, 否则可被伪造)"
            : "TCP 对端(不信任 X-Forwarded-For)");

    // 控制面(agent_cli 出站连这里)
    fprintf(stderr, "  控制面    hub(JZNet) %s:%d  token=%s\n", config.hub.bind.c_str(),
            config.hub.port, config.hub.token.empty() ? "(无)" : "(已启用)");

    // 日志: 目录 + 固定文件名(Server.log); 按天轮转时是 Server-yyyyMMdd.log
    fprintf(stderr, "  日志      %s/Server%s.log%s\n",
            jzLogDirOf(config.logFile, JZPlatform::applicationDirPath() + "/data/logs").c_str(),
            config.logRotateDaily ? "-yyyyMMdd" : "",
            config.logRotateDaily ? "  (按天轮转)" : "");

    // agents
    if (config.agents.empty())
        fprintf(stderr, "  Agents    无(不管理任何 agent_cli)\n");
    else
    {
        fprintf(stderr, "  Agents    %d 个:\n", (int)config.agents.size());
        for (const auto &ac : config.agents)
            fprintf(stderr, "    - %s  %s  [inproc] %s%s\n", ac.name.c_str(),
                    ac.enabled ? "[自启]" : "[手动]",
                    ac.args.empty() ? "(默认 config.json)" : "",
                    ac.args.empty() ? "" : joinStrings(ac.args, " ").c_str());
    }
}

int main(int argc, char *argv[])
{
    JZCommandLineParser parser;
    parser.setDescription("JZAgentCliServer - agent_cli 管理服务器(控制面 + 管理台)\n");

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

    JZCliServerConfig config;
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

    // 日志: General 模块落盘到 <logFile目录或 exeDir/data/logs>/Server.log。
    // logFile 一律按**目录**解释(与 agent_cli / agent_node 同一口径, 见 jzLogDirOf):
    // 老配置里写了 data/server.log 这种完整文件名也认, 只取目录。
    // 之前 server 根本没初始化 JZLogManager, 所有 jzInfo/jzWarning 全是空转 ——
    // 排查"任务下发/续会话/agent 上下线"时什么都看不到。
    {
        const string logDir = jzLogDirOf(config.logFile,
                                         JZPlatform::applicationDirPath() + "/data/logs");
        JZDir().mkpath(logDir);

        JZLogConfig lc;
        lc.shouldPrintTime = true;
        lc.logPath = logDir;
        lc.rotateDaily = config.logRotateDaily;
        JZLogManager::instance()->setConfig(lc);

        JZLogModuleConfig general;
        general.name = "Server";                       // 落到 Server.log / Server-yyyyMMdd.log
        general.type = JZLogModuleConfig::File;
        JZLogManager::instance()->setModuleConfig(LogModule_General, general);

        // inproc 的 cli 与本进程共用 JZLog: 它的回合诊断(LogModule_Agent)也落这里。
        // ⚠ logPath 是进程级全局量, 所以 inproc cli 的日志与 Server.log 同目录 ——
        //    配置里各 cli 的 logFile 只对"独立进程模式"有意义。
        if (!config.agents.empty())
        {
            JZLogModuleConfig agentLog;
            agentLog.name = "Agent";                   // 落到 Agent.log / Agent-yyyyMMdd.log
            agentLog.type = JZLogModuleConfig::File;
            JZLogManager::instance()->addModule(LogModule_Agent, agentLog);
            JZLogManager::instance()->setModuleConfig(LogModule_Agent, agentLog);
        }
    }

    // 优雅退出: SIGTERM/SIGINT 不要直接死进程 —— 那样 C++ 析构不跑, 被托管的 agent_cli
    // 会变孤儿(实测孤儿还会带着同一个 agentId 反复重连, 和重启后的新进程互相顶替, 一秒一次)。
    // 这里信号处理器只置一个标志(异步信号安全), 由事件循环里的定时器转成 app.quit():
    // 正常退出后 JZAgentCliServer / JZProcess 的析构会把子进程整棵树收掉。
    static volatile sig_atomic_t g_stopRequest = 0;
    std::signal(SIGTERM, [](int) { g_stopRequest = 1; });
    std::signal(SIGINT, [](int) { g_stopRequest = 1; });
    {
        JZTimer *watch = new JZTimer();
        connect(&watch->sigTimeout, []()
                {
                    if (!g_stopRequest)
                        return;
                    g_stopRequest = 0;
                    jzInfo() << "收到退出信号, 正在停止托管的 agent...";
                    jzApp->quit();
                });
        watch->start(200);
    }

    JZAgentCliServer server;
    if (!server.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // 控制面 + 后台网页/API + 拉起 enabled 的 agent。
    // 端口没起来(占用 / bind 地址不对)就当启动失败退出: 半死不活的进程最难查 ——
    // 看起来"在跑", 实际上谁也连不上, HTTPS 下还会被误判成证书问题。
    string startError;
    if (!server.start(startError))
    {
        fprintf(stderr, "%s\n", startError.c_str());
        return 1;
    }

    jzDebug("JZAgentCliServer 就绪: %s=%s:%d  hub=%s:%d  agents=%d",
            config.http.sslEnabled ? "https" : "http", config.http.bind.c_str(), config.http.port,
            config.hub.bind.c_str(), config.hub.port, (int)config.agents.size());

    return app.exec();
}
