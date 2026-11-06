// JZAgentCliServer —— agent_cli 管理服务器
//
//   1) (可选) 跑 frps 服务端, 与 agent_cli 的 frpc client 配对做内网穿透;
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
#include "JZPlatform.h"
#include "JZString.h"
#include "JZDebug.h"

using namespace std;

// ===========================================================================
// 默认配置(--init 写出的文本)。
//   http.*     后台网页 / 管理 API 的监听地址与鉴权 token。
//   frp.server.* frps 服务端配置(可选; 只给"现场自带 frpc"的部署用, 与 hub 无关)。
//   agents[]   被管理的一系列 agent_cli(各作为一个子进程拉起并监管)。
// ===========================================================================
static const char *kDefaultConfigJson = R"json({
    "http": {
        "bind":   "127.0.0.1",
        "port":   8088,
        "token":  ""
    },
    "hub": {
        "bind":  "0.0.0.0",
        "port":  8090,
        "token": ""
    },
    "frp": {
        "server": {
            "enabled":  false,
            "bindPort": 7000,
            "token":    ""
        }
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
// 分层配置取值: 优先 "frp.server.<name>", 回落顶层 legacy("<legacy>")。
// ---------------------------------------------------------------------------
static const Json::Value *findMember(const Json::Value &root, const string &group,
                                     const string &name, const string &legacy)
{
    // name 支持 "a.b.c" 多级嵌套(如 frp.server.enabled -> root["frp"]["server"]["enabled"])
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
    readString(root, "http", "token", "httpToken", config.http.token);

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

    // frp 服务端(frps); 兼容旧平铺键 frpEnabled / frpBindPort / frpToken
    readBool(root, "frp", "server.enabled", "frpEnabled", config.frpEnabled);
    readInt(root, "frp", "server.bindPort", "frpBindPort", config.frpBindPort);
    readString(root, "frp", "server.token", "frpToken", config.frpToken);

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
    return true;
}

static void printConfig(const string &configPath, const JZCliServerConfig &config)
{
    // 打到 stderr: stdout 留给将来扩展(如 ACP 通道)
    fprintf(stderr, "配置文件: %s\n", configPath.c_str());
    fprintf(stderr, "  角色      agent_cli 管理服务器(frps 可选 + 后台网页 + agent 监管)\n");

    // http 网页 / API
    fprintf(stderr, "  网页      http://%s:%d  token=%s\n", config.http.bind.c_str(),
            config.http.port, config.http.token.empty() ? "(无)" : "(已启用)");

    // 控制面(agent_cli 出站连这里)
    fprintf(stderr, "  控制面    hub(JZNet) %s:%d  token=%s\n", config.hub.bind.c_str(),
            config.hub.port, config.hub.token.empty() ? "(无)" : "(已启用)");

    // frp 服务端
    if (!config.frpEnabled)
        fprintf(stderr, "  FRP       关(设 frp.server.enabled=true 启动 frps)\n");
    else
        fprintf(stderr, "  FRP 服务端  frps 监听 %d, token=%s\n", config.frpBindPort,
                config.frpToken.empty() ? "(无)" : "(已启用)");

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
    parser.setDescription("JZAgentCliServer - FRP server (frps)\n");

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
    JZAgentCliServer server;
    if (!server.init(config, err))
    {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    jzDebug("JZAgentCliServer 就绪: http=%s:%d  hub=%s:%d  frps=%s  agents=%d",
            config.http.bind.c_str(), config.http.port,
            config.hub.bind.c_str(), config.hub.port,
            config.frpEnabled ? "开" : "关", (int)config.agents.size());
    server.start();   // frps(可选) + 后台网页/API + 拉起 enabled 的 agent

    return app.exec();
}
