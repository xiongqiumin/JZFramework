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

#include <fstream>
#include <sstream>
#include <string>

#include "JZAgentCliServer.h"
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
// 默认配置(--init 写出的文本)。
//   http.*     后台网页 / 管理 API 的监听地址(鉴权在传输层: mTLS 客户端证书, 没有 token)。
//   http.ssl.* HTTPS 默认开启且默认校验客户端证书(mTLS); 证书路径相对配置文件目录。
//              要跑起来必须先备好 certs/{server.crt,server.key,ca.crt}(生成方法见
//              docs/agent-hub-design.md §7.1); 缺文件时 server 直接启动失败, 不会退回明文。
//   agents[]   被管理的一系列 agent_cli(各作为一个子进程拉起并监管)。
//   内网穿透不在本进程内: 需要时用独立的 jzfrps / jzfrpc 进程。
// ===========================================================================
static const char *kDefaultConfigJson = R"json({
    "http": {
        "bind":   "127.0.0.1",
        "port":   8088,
        "ssl": {
            "enabled":           true,
            "certFile":          "certs/server.crt",
            "keyFile":           "certs/server.key",
            "caFile":            "certs/ca.crt",
            "requireClientCert": true
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
            "cmd":     "agent_cli",
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

    // http.ssl: HTTPS(默认开) + 客户端证书校验(默认开)。证书路径相对配置文件目录。
    readBool(root, "http", "ssl.enabled", "httpSslEnabled", config.http.sslEnabled);
    readString(root, "http", "ssl.certFile", "httpSslCert", config.http.certFile);
    readString(root, "http", "ssl.keyFile", "httpSslKey", config.http.keyFile);
    readString(root, "http", "ssl.caFile", "httpSslCa", config.http.caFile);
    readBool(root, "http", "ssl.requireClientCert", "httpSslRequireClientCert",
             config.http.requireClientCert);

    if (config.http.certFile.empty())
        config.http.certFile = "certs/server.crt";
    if (config.http.keyFile.empty())
        config.http.keyFile = "certs/server.key";
    if (config.http.caFile.empty())
        config.http.caFile = "certs/ca.crt";

    const string cfgDir = dirName(path);

    // logFile: 空 = <exeDir>/data/logs; 相对路径按"配置文件所在目录"解析
    if (root.isMember("logFile") && root["logFile"].isString() && !root["logFile"].asString().empty())
        config.logFile = resolvePath(cfgDir, root["logFile"].asString());

    config.http.certFile = resolvePath(cfgDir, config.http.certFile);
    config.http.keyFile = resolvePath(cfgDir, config.http.keyFile);
    config.http.caFile = resolvePath(cfgDir, config.http.caFile);

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
            as.readMemberDefault("cmd", ac.cmd, string("agent_cli"));
            as.readMemberDefault("dir", ac.dir, string());
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
    const Json::Value *self = findMember(root, "", "selfAgent", "selfAgent");
    if (self && self->isObject())
    {
        JZJsonStream as(self);
        as.readMemberDefault("enabled", config.selfAgent.enabled, false);
        as.readMemberDefault("agentId", config.selfAgent.agentId, string("server"));
        as.readMemberDefault("name", config.selfAgent.name, string("server(内置)"));
        as.readMemberDefault("rootDir", config.selfAgent.rootDir, string());
        as.readMemberDefault("modelUrl", config.selfAgent.modelUrl, string());
        as.readMemberDefault("modelName", config.selfAgent.modelName, string());
        as.readMemberDefault("modelToken", config.selfAgent.modelToken, string());
        as.readMemberDefault("netTimeout", config.selfAgent.netTimeout, 120000);
        as.readMemberDefault("maxRounds", config.selfAgent.maxRounds, 40);
        as.readMemberDefault("maxToken", config.selfAgent.maxToken, 100000);
        as.readMemberDefault("systemExtra", config.selfAgent.systemExtra, string());

        if (config.selfAgent.enabled && config.selfAgent.modelUrl.empty())
        {
            error = "selfAgent.enabled=true 但没配 selfAgent.modelUrl";
            return false;
        }
        if (config.selfAgent.enabled && config.selfAgent.agentId.empty())
            config.selfAgent.agentId = "server";
    }
    return true;
}

static void printConfig(const string &configPath, const JZCliServerConfig &config)
{
    // 打到 stderr: stdout 留给将来扩展(如 ACP 通道)
    fprintf(stderr, "配置文件: %s\n", configPath.c_str());
    fprintf(stderr, "  角色      agent_cli 管理服务器(控制面 + 后台网页 + agent 监管)\n");

    // http 网页 / API(鉴权 = mTLS 客户端证书, 没有 token)
    fprintf(stderr, "  网页      %s://%s:%d\n",
            config.http.sslEnabled ? "https" : "http", config.http.bind.c_str(),
            config.http.port);
    if (config.http.sslEnabled)
    {
        fprintf(stderr, "    TLS     证书=%s[%s]  私钥=%s[%s]\n",
                config.http.certFile.c_str(), JZFileInfo::exists(config.http.certFile) ? "在" : "缺失",
                config.http.keyFile.c_str(), JZFileInfo::exists(config.http.keyFile) ? "在" : "缺失");
        if (config.http.requireClientCert)
            fprintf(stderr, "    客户端证书校验=开  CA=%s[%s]\n", config.http.caFile.c_str(),
                    JZFileInfo::exists(config.http.caFile) ? "在" : "缺失");
        else
            fprintf(stderr, "    客户端证书校验=关(单向 TLS)\n");
    }
    else
        fprintf(stderr, "    ⚠ HTTPS 已关闭: 明文 HTTP(仅限完全可信的隔离网络)\n");

    // 控制面(agent_cli 出站连这里)
    fprintf(stderr, "  控制面    hub(JZNet) %s:%d  token=%s\n", config.hub.bind.c_str(),
            config.hub.port, config.hub.token.empty() ? "(无)" : "(已启用)");

    // agents
    if (config.agents.empty())
        fprintf(stderr, "  Agents    无(不管理任何 agent_cli)\n");
    else
    {
        fprintf(stderr, "  Agents    %d 个:\n", (int)config.agents.size());
        for (const auto &ac : config.agents)
            fprintf(stderr, "    - %s  %s  %s %s\n", ac.name.c_str(),
                    ac.enabled ? "[自启]" : "[手动]",
                    ac.cmd.c_str(),
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
    // 之前 server 根本没初始化 JZLogManager, 所有 jzInfo/jzWarning 全是空转 ——
    // 排查"任务下发/续会话/agent 上下线"时什么都看不到。
    {
        const string logDir = config.logFile.empty()
                                  ? (JZPlatform::applicationDirPath() + "/data/logs")
                                  : dirName(config.logFile);
        JZDir().mkpath(logDir);

        JZLogConfig lc;
        lc.shouldPrintTime = true;
        lc.logPath = logDir;
        JZLogManager::instance()->setConfig(lc);

        JZLogModuleConfig general;
        general.name = "Server";                       // 落到 Server.log
        general.type = JZLogModuleConfig::File;
        JZLogManager::instance()->setModuleConfig(LogModule_General, general);
    }

    JZAgentCliServer server;
    if (!server.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    jzDebug("JZAgentCliServer 就绪: %s=%s:%d  hub=%s:%d  agents=%d",
            config.http.sslEnabled ? "https" : "http", config.http.bind.c_str(), config.http.port,
            config.hub.bind.c_str(), config.hub.port, (int)config.agents.size());
    server.start();   // 控制面 + 后台网页/API + 拉起 enabled 的 agent

    return app.exec();
}
