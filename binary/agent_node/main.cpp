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
    "    \"nodeName\":      \"agent_node\",\n"
    "    \"version\":       \"1.0.0\",\n"
    "    \"port\":          39000,\n"
    "    \"rootDir\":       \"\",\n"
    "    \"trustedDirs\":   [],\n"
    "    \"shellProgram\":  \"\",\n"
    "    \"recycleSec\":    900,\n"
    "    \"allowTerminal\": true,\n"
    "    \"logFile\":       \"\"\n"
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
    js.readMemberDefault("port", config.port, config.port);
    js.readMemberDefault("rootDir", config.rootDir, config.rootDir);
    js.readMemberDefault("shellProgram", config.shellProgram, config.shellProgram);
    js.readMemberDefault("allowTerminal", config.allowTerminal, config.allowTerminal);
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

int main(int argc, char *argv[])
{
    JZCommandLineParser parser;
    parser.setDescription("agent_node - 被控端 MCP 节点\n");

    string cmdConfig;
    parser.addOption(JZCommandLineOption("--config", "-c",
        "配置文件路径(默认 exe 同目录 config.json)"), &cmdConfig);

    bool cmdInit = false;
    parser.addOption(JZCommandLineOption("--init", "-i",
        "生成默认 config.json 后退出(不启动服务)"), &cmdInit);

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

    JZApplication app;
    JZAgentNode node;
    node.init(config);

    if (!node.server()->listenHttp("0.0.0.0", config.port))
    {
        jzError().nospace() << "监听失败, port=" << config.port << "(端口被占用?)";
        return 1;
    }

    jzInfo().nospace() << "节点已就绪: http://<本机IP>:" << config.port << "/mcp   节点名=" << config.nodeName;

    return app.exec();
}
