// agent_node —— 被控端 MCP 节点
//
// 装在远程机器上, 把"这台机器能做的事"以 MCP 工具的形式暴露出去, 供本地大模型调用。
// 本地侧用现成的 JZMcpClient 连上来, JZAgent::addMcp() 之后远端工具就和本地工具
// 混在同一张表里, 对模型完全透明。
//
// 见 JZAgentNode.h / JZTerminalTools.h 头部注释。
//
// 配置全部放在 config.json(默认 exe 同目录, 可用 -c 指定), 不再通过命令行传参。
// 命令行只负责: -c 指定配置文件, --init 生成默认配置文件, -h 看帮助。

#include <stdio.h>
#include <stdlib.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "JZAgentNode.h"
#include "JZApplication.h"
#include "JZCodeTools.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZFileInfo.h"
#include "JZLog.h"
#include "JZMcpServer.h"
#include "JZString.h"
#include "JZPlatform.h"
#include "JZCommandLineParser.h"
#include "JZJson.h"

using namespace std;

// 默认配置(与 binary/agent_node/config.json 模板保持一致)。
// --init 把它写出来; loadConfig 也是按这些字段读的。
static const char *kDefaultConfigJson =
    "{\n"
    "    \"nodeName\":           \"agent_node\",\n"
    "    \"version\":            \"1.0.0\",\n"
    "    \"host\":               \"0.0.0.0\",\n"
    "    \"port\":               7500,\n"
    "    \"rootDir\":            \"\",\n"
    "    \"trustedDirs\":        [],\n"
    "    \"allowTerminal\":      true,\n"
    "    \"allowFileTools\":     true,\n"
    "    \"allowPackageManager\": false,\n"
    "    \"allowNetwork\":       false,\n"
    "    \"shellProgram\":       \"\",\n"
    "    \"maxSessions\":        8,\n"
    "    \"recycleSec\":         900,\n"
    "    \"logFile\":            \"\"\n"
    "}\n";

static string pathJoin(const string &dir, const string &name)
{
    if (dir.empty())
        return name;
    char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + name;
    return dir + "/" + name;
}

// "D:/sdk:rw" -> {path:"D:/sdk", writable:true}
// Windows 盘符里的冒号不能当分隔符, 所以只看最后一个冒号且后缀必须是 ro/rw
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
    if(JZFileInfo::exists(path))
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

static bool loadConfig(const string &path, JZNodeConfig &config, string &error)
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

    JZJsonStream js(&root);
    js.readMemberDefault("nodeName", config.nodeName, config.nodeName);
    js.readMemberDefault("version", config.version, config.version);
    js.readMemberDefault("host", config.host, config.host);
    js.readMemberDefault("port", config.port, config.port);
    js.readMemberDefault("rootDir", config.rootDir, config.rootDir);
    js.readMemberDefault("shellProgram", config.shellProgram, config.shellProgram);
    js.readMemberDefault("allowTerminal", config.allowTerminal, config.allowTerminal);
    js.readMemberDefault("allowFileTools", config.allowFileTools, config.allowFileTools);
    js.readMemberDefault("allowPackageManager", config.allowPackageManager, config.allowPackageManager);
    js.readMemberDefault("allowNetwork", config.allowNetwork, config.allowNetwork);
    js.readMemberDefault("maxSessions", config.maxSessions, config.maxSessions);
    js.readMemberDefault("recycleSec", config.recycleSec, config.recycleSec);
    js.readMemberDefault("logFile", config.logFile, config.logFile);

    const Json::Value &arr = root["trustedDirs"];
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

// 打印"生效配置"(空值在这里补成默认值), 远程部署时先跑一次 --check:
// 看清楚这个节点到底把哪儿当工作区、开了哪些口子, 比起来之后从工具报错里猜快得多。
static void printConfig(const string &configPath, const JZNodeConfig &config)
{
    string trusted;
    for (size_t i = 0; i < config.trustedDirs.size(); i++)
    {
        if (i)
            trusted += ", ";
        trusted += config.trustedDirs[i].path + (config.trustedDirs[i].writable ? "(rw)" : "(ro)");
    }

    jzInfo().noquote().nospace() << "配置文件: " << configPath;
    jzInfo().noquote().nospace() << "  节点名    " << config.nodeName << " v" << config.version;
    jzInfo().noquote().nospace() << "  监听      " << config.host << ":" << config.port << "/mcp";
    jzInfo().noquote().nospace() << "  工作区    " << (config.rootDir.empty()
        ? JZDir::currentPath() + "  (配置为空, 取进程当前目录)" : config.rootDir);
    jzInfo().noquote().nospace() << "  授信目录  " << (trusted.empty() ? string("(无)") : trusted);
    jzInfo().noquote().nospace() << "  工具      终端=" << (config.allowTerminal ? "开" : "关")
                                 << "  文件=" << (config.allowFileTools ? "开" : "关");
    jzInfo().noquote().nospace() << "  命令策略  包管理=" << (config.allowPackageManager ? "开" : "关")
                                 << "  联网=" << (config.allowNetwork ? "开" : "关");
    jzInfo().noquote().nospace() << "  会话      上限=" << config.maxSessions
                                 << "  空闲回收=" << (config.recycleSec > 0 ? to_string(config.recycleSec) + "s" : string("关"));
    jzInfo().noquote().nospace() << "  shell     " << (config.shellProgram.empty() ? string("自动探测") : config.shellProgram);
    jzInfo().noquote().nospace() << "  日志文件  " << (config.logFile.empty() ? string("(只打控制台)") : config.logFile);
}

int main(int argc, char *argv[])
{
    JZCommandLineParser parser;
    parser.setDescription("agent_node - 被控端 MCP 节点\n");

    string cmdConfig;
    parser.addOption(JZCommandLineOption("--config", "-c",
        "配置文件路径(默认 exe 同目录 config.json)"), &cmdConfig);

    bool cmdInit = false;
    parser.addOption(JZCommandLineOption("--init", "",
        "生成默认 config.json 后退出(不启动服务)"), &cmdInit);

    bool cmdCheck = false;
    parser.addOption(JZCommandLineOption("--check", "",
        "校验配置并打印生效配置后退出(不启动服务)"), &cmdCheck);

    if (!parser.parse(argc, argv))
        return 1;   // 用法不合法(parser 内部已打印错误并退出)

    string configPath = cmdConfig.empty()
        ? pathJoin(JZPlatform::applicationDirPath(), "config.json")
        : cmdConfig;

    if (cmdInit)
    {
        string err;
        if (!writeDefaultConfig(configPath, err))
        {
            jzError() << err;
            return 1;
        }
        jzInfo().nospace() << "已生成默认配置文件: " << configPath;
        return 0;
    }

    JZNodeConfig config;
    string err;
    if (!loadConfig(configPath, config, err))
    {
        jzError() << err;
        return 1;
    }

    if (config.port <= 0 || config.port > 65535)
    {
        jzError().nospace() << "端口不合法: " << config.port;
        return 1;
    }
    if (config.host.empty())
        config.host = "0.0.0.0";
    if (config.maxSessions < 0)
        config.maxSessions = 0;

    // 配置没问题(不启服务): 打印生效配置, 让部署的人确认一遍
    if (cmdCheck)
    {
        printConfig(configPath, config);
        return 0;
    }

    JZApplication app;

    // 日志落文件: 直接走 JZLogManager 自带的 File 输出(它有定时器定期 flush,
    // 见 JZLogManager::updateFlushTimer)。⚠ 必须晚于 JZApplication —— 定时器要
    // 挂到 app 的事件循环上, 没有 app 就起不来。
    if (!config.logFile.empty())
    {
        JZFileInfo info(config.logFile);
        string logDir = info.path();
        if (logDir.empty())
            logDir = ".";
        if (!JZFileInfo::exists(logDir) && !JZDir().mkpath(logDir))
        {
            jzError().nospace() << "无法创建日志目录: " << logDir;
            return 1;
        }

        // JZLogManager 的文件名是 "<logPath>/<模块名>.log", 所以把用户给的路径拆成
        // 目录 + 不带后缀的模块名(/var/log/agent_node.log -> /var/log + agent_node)。
        JZLogConfig logConfig;
        logConfig.shouldSaveToFile = true;
        logConfig.logPath = logDir;

        JZLogModuleConfig moduleConfig = JZLogManager::instance()->moduleConfig(LogModule_General);
        moduleConfig.name = info.baseName();
        moduleConfig.type = JZLogModuleConfig::Console | JZLogModuleConfig::File;

        JZLogManager::instance()->setConfig(logConfig);
        JZLogManager::instance()->setModuleConfig(LogModule_General, moduleConfig);
    }

    JZAgentNode node;
    node.init(config);

    if (!node.server()->listenHttp(config.host, config.port))
    {
        jzError().nospace() << "监听失败 " << config.host << ":" << config.port << "(端口被占用 / 地址不可用?)";
        return 1;
    }

    string showHost = (config.host == "0.0.0.0") ? string("<本机IP>") : config.host;
    jzInfo().noquote().nospace() << "节点已就绪: http://" << showHost << ":" << config.port
                                 << "/mcp   节点名=" << config.nodeName;

    return app.exec();
}
