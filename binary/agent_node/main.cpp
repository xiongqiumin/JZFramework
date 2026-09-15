// agent_node —— 被控端 MCP 节点
//
// 装在远程机器上, 把"这台机器能做的事"以 MCP 工具的形式暴露出去, 供本地大模型调用。
// 本地侧用现成的 JZMcpClient 连上来, JZAgent::addMcp() 之后远端工具就和本地工具
// 混在同一张表里, 对模型完全透明。
//
// 见 JZAgentNode.h / JZTerminalTools.h 头部注释。

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "JZAgentNode.h"
#include "JZApplication.h"
#include "JZCodeTools.h"
#include "JZDebug.h"
#include "JZMcpServer.h"
#include "JZString.h"

using namespace std;

static void printUsage()
{
    printf("agent_node - 被控端 MCP 节点(让本地大模型操作这台机器)\n\n");
    printf("用法: agent_node [选项]\n\n");
    printf("传输:\n");
    printf("  --http <port>         监听 Streamable HTTP, 端点 /mcp。默认, 端口 39000\n");
    printf("  --ws <port>           监听 WebSocket\n\n");
    printf("配置:\n");
    printf("  --name <name>         节点名。**多节点时必须唯一** —— 本地工具名前缀就是它\n");
    printf("  --root <dir>          工作区根目录(相对路径以它为基准), 默认进程当前工作目录\n");
    printf("  --trusted <dir[:rw]>  追加授信目录(默认只读; 末尾加 :rw 表示可写), 可重复\n");
    printf("  --shell <path>        跑命令用的 shell, 默认自动探测\n");
    printf("                        (POSIX=/bin/sh; Windows=Git Bash 的 bash.exe, 找不到直接报错)\n");
    printf("  --recycle <sec>       会话空闲多久回收, 默认 900 秒, 0=不回收\n");
    printf("  --no-terminal         不注册终端工具(⚠ 那就一个工具都不剩了)\n");
    printf("  -h, --help            显示本帮助\n\n");
    printf("本地侧连法:\n");
    printf("  auto *c = new JZMcpClient();\n");
    printf("  c->setToolPrefix(\"<节点名>__\");          // 默认就是它, 名字唯一即可\n");
    printf("  c->setAutoReconnect(true);\n");
    printf("  c->startHttp(\"http://<这台机器>:<port>/mcp\");\n");
    printf("  // sigFinished 里 isReady() 之后: agent->addMcp(c);\n");
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

int main(int argc, char *argv[])
{
    JZNodeConfig config;
    string mode = "http";
    int port = 39000;

    for (int i = 1; i < argc; i++)
    {
        string a = argv[i];
        auto nextValue = [&](const char *what) -> string
        {
            if (i + 1 >= argc)
            {
                printf("选项 %s 缺少参数值\n", what);
                exit(1);
            }
            return argv[++i];
        };

        if (a == "--http")
        {
            mode = "http";
            port = atoi(nextValue("--http").c_str());
        }
        else if (a == "--ws")
        {
            mode = "ws";
            port = atoi(nextValue("--ws").c_str());
        }
        else if (a == "--name")
            config.nodeName = nextValue("--name");
        else if (a == "--root")
            config.rootDir = nextValue("--root");
        else if (a == "--trusted")
        {
            TrustedDir dir;
            if (!parseTrusted(nextValue("--trusted"), dir))
            {
                printf("--trusted 的目录为空\n");
                return 1;
            }
            config.trustedDirs.push_back(dir);
        }
        else if (a == "--shell")
            config.shellProgram = nextValue("--shell");
        else if (a == "--recycle")
            config.recycleSec = atoi(nextValue("--recycle").c_str());
        else if (a == "--no-terminal")
            config.allowTerminal = false;
        else if (a == "--log")
            config.logFile = nextValue("--log");
        else if (a == "-h" || a == "--help")
        {
            printUsage();
            return 0;
        }
        else
        {
            printf("未知参数: %s\n\n", a.c_str());
            printUsage();
            return 1;
        }
    }

    if (port <= 0 || port > 65535)
    {
        printf("端口不合法: %d\n", port);
        return 1;
    }

    JZApplication app;

    JZAgentNode node;
    node.init(config);

    bool listening = false;
    if (mode == "http")
        listening = node.server()->listenHttp("0.0.0.0", port);
    else
        listening = node.server()->listenWebSocket("0.0.0.0", port);

    if (!listening)
    {
        jzError().nospace() << "监听失败, port=" << port << "(端口被占用?)";
        return 1;
    }

    if (mode == "http")
    {
        jzInfo().nospace() << "节点已就绪: http://<本机IP>:" << port << "/mcp   节点名=" << config.nodeName;
    }
    else
    {
        jzInfo().nospace() << "节点已就绪: ws://<本机IP>:" << port << "   节点名=" << config.nodeName;
    }

    return app.exec();
}
