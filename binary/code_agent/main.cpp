// JZCodeAgent —— 基于 JZAgent + JZHttpServer 的本地代码助手
//
// 启动后在浏览器打开 http://127.0.0.1:<port> 即可使用。
// 大模型通过 OpenAI 兼容接口调用（DashScope / vLLM / Ollama 等均可）。
//
// 常用参数:
//   --port        监听端口，默认 18080
//   --workspace   工作区根目录，默认当前目录（所有文件操作都限制在此目录内）
//   --modelUrl    OpenAI 兼容的 chat/completions 地址
//   --modelName   模型名
//   --modelToken  API Key
//   --build       默认编译命令，默认 "cmake --build build --config Release"
//   --run         默认运行命令

#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZDebug.h"
#include "JZDir.h"

#include "JZCodeAgent.h"
#include "JZCodeAgentServer.h"
#include "JZWebView.h"

using namespace std;

int main(int argc, char *argv[])
{
    // CEF 子进程（渲染/GPU 等）会以相同 exe 重新启动，必须最先交给 CEF 处理。
    // 返回 >=0 表示当前进程是子进程且已自行退出，这里直接 return。
#ifdef HAS_JZWebView
    int cefSub = JZWebView::preMain(argc, argv);
    if (cefSub >= 0)
        return cefSub;
#endif

    JZApplication app;

    JZAgentConfig llm;
    llm.modelUrl = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    llm.modelName = "qwen-plus";
    llm.modelToken = "";
    llm.netTimeout = 120000;
    llm.maxRounds = 100;

    CodeAgentConfig code;
    code.workspace = JZDir::currentPath();
#ifdef _WIN32
    code.buildCommand = "cmake --build build --config Release";
#else
    code.buildCommand = "cmake --build build";
#endif
    code.runCommand = "";

    int port = 18080;

    JZCommandLineParser parser;
    parser.setDescription("JZCodeAgent - 基于 JZAgent 的本地代码助手（网页版）");
    parser.addOption("--port", &port);
    parser.addOption("--workspace", &code.workspace);
    parser.addOption("--modelUrl", &llm.modelUrl);
    parser.addOption("--modelName", &llm.modelName);
    parser.addOption("--modelToken", &llm.modelToken);
    parser.addOption("--build", &code.buildCommand);
    parser.addOption("--run", &code.runCommand);

    if (!parser.parse(argc, argv))
    {
        jzError() << parser.errorString();
        parser.printHelp();
        return 1;
    }

    JZDir workspace(code.workspace);
    if (!workspace.exists())
    {
        jzError() << "工作区目录不存在: " << code.workspace;
        return 1;
    }

    JZCodeAgent agent;
    agent.init(llm, code);

    JZCodeAgentServer server;
    if (!server.start(&agent, port))
        return 1;

    jzInfo() << "==================================================";
    jzInfo().noquote() << " JZCodeAgent 已启动: http://127.0.0.1:" + to_string(port);
    jzInfo() << " 工作区  : " << agent.codeConfig().workspace;
    jzInfo() << " 模型    : " << agent.llmConfig().modelName << " (" << agent.llmConfig().modelUrl << ")";
    jzInfo() << " 编译命令: " << agent.codeConfig().buildCommand;
    jzInfo() << " 运行命令: " << agent.codeConfig().runCommand;
    jzInfo() << "==================================================";

    // 用 JZWebView(libcef) 窗口承载前端（启动即弹出）
    JZWebView webview;

    std::string url = "http://127.0.0.1:" + to_string(port);

    // 窗口关闭后（sigClosed 在窗口彻底销毁后发出）退出主循环
    connect(&webview.sigClosed, [&app]() { app.exit(0); });

    webview.open(url, "JZCodeAgent - " + url, nullptr);

    return app.exec();
}
