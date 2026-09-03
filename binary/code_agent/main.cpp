// JZCodeAgent —— 基于 JZAgent + JZHttpServer 的本地代码助手
//
// 启动后在浏览器打开 http://127.0.0.1:<port> 即可使用。
// 大模型通过 OpenAI 兼容接口调用（DashScope / vLLM / Ollama 等均可）。
//
// 所有运行参数（模型、工作区、端口、语音服务等）不再通过命令行传入，
// 改由网页设置面板读写，并持久化到本地的 code_agent.json（位于 exe 所在目录）。
//
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZDir.h"

#include "JZCodeAgent.h"
#include "JZCodeAgentServer.h"
#include "JZSpeechClient.h"
#include "JZDesktopServices.h"

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

    // 默认配置（首次运行、本地无配置文件时使用）。之后可在网页设置面板修改并保存。
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

    JZCodeAgent agent;
    agent.init(llm, code);

    // 配置持久化到 exe 所在目录的 code_agent.json，启动前先把文件里的覆盖项应用进来
    JZCodeAgentServer server;
    server.setAgent(&agent);
    server.setConfigPath(app.applicationDirPath() + "/code_agent.json");
    server.loadConfig();

    if (!server.start(&agent, server.port()))
        return 1;

    // 语音客户端: 对接 ugreen-server 的 ASR/TTS (网页录音/播放能力)
    JZSpeechClient speech;
    speech.setLocalPlay(server.localPlay());
    if (speech.connectServer(server.speechWs()))
        jzInfo() << " 语音服务已连接: " << server.speechWs();
    else
        jzWarning() << " 语音服务连接失败: " << server.speechWs()
                     << " (ASR/TTS 不可用, 其余功能正常)";
    server.setSpeechClient(&speech);

    jzInfo() << "==================================================";
    jzInfo().noquote() << " JZCodeAgent 已启动: http://127.0.0.1:" + to_string(server.port());
    jzInfo() << " 工作区  : " << agent.codeConfig().workspace;
    jzInfo() << " 模型    : " << agent.llmConfig().modelName << " (" << agent.llmConfig().modelUrl << ")";
    jzInfo() << " 编译命令: " << agent.codeConfig().buildCommand;
    jzInfo() << " 运行命令: " << agent.codeConfig().runCommand;
    jzInfo() << " 语音    : " << server.speechWs();
    jzInfo() << "==================================================";

    // 启动后用系统默认浏览器打开网页版 UI
    {
        std::string url = "http://127.0.0.1:" + std::to_string(server.port());
        if (JZDesktopServices::openUrl(url))
            jzInfo().noquote() << " 已在浏览器打开: " + url;
        else
            jzWarning().noquote() << " 无法自动打开浏览器, 请手动访问: " + url;
    }

    return app.exec();
}
