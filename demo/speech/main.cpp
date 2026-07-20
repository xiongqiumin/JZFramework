#include <string>
#include <vector>
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZConsoleInput.h"
#include "JZAudioDeviceInfo.h"
#include "JZSpeechClient.h"
#include "JZSpeechCore.h"
#include "JZSpeechDemo.h"

static void printUsage(const char *prog)
{
    jzDebug() << "用法: " << prog
              << " [wsUrl] [--in <id>] [--out <id>] [--novad] [--nokws] [--list] [-h]";
    jzDebug() << "  wsUrl  默认为 ws://127.0.0.1:18000";
    jzDebug() << "  --in    指定音频输入设备 id (见 --list)";
    jzDebug() << "  --out   指定音频输出设备 id";
    jzDebug() << "  --novad 关闭 VAD (全程上行)";
    jzDebug() << "  --nokws 关闭唤醒词 (始终处理)";
    jzDebug() << "  --list  列出音频设备后退出";
}

int app_main(int argc, char *argv[])
{
    JZApplication app;

    std::string url = "ws://127.0.0.1:18000";
    std::string inDev, outDev;
    bool novad = false;
    bool nokws = false;
    bool listOnly = false;

    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string &a = args[i];
        if (a == "--in" && i + 1 < args.size())       inDev  = args[++i];
        else if (a == "--out" && i + 1 < args.size()) outDev = args[++i];
        else if (a == "--novad")                      novad = true;
        else if (a == "--nokws")                      nokws = true;
        else if (a == "--list")                       listOnly = true;
        else if (a == "-h" || a == "--help")          { printUsage(argv[0]); return 0; }
        else if (!a.empty() && a[0] != '-')           url = a;
    }

    if (listOnly)
    {
        jzDebug() << "==== 输入设备 ====";
        for (auto &d : JZAudioDeviceInfo::inputList())
            jzDebug() << "  [" << d.id.c_str() << "] " << d.name.c_str();
        jzDebug() << "==== 输出设备 ====";
        for (auto &d : JZAudioDeviceInfo::outputList())
            jzDebug() << "  [" << d.id.c_str() << "] " << d.name.c_str();
        return 0;
    }

    JZEventLoop loop;

    // ---- 音频 I/O (JZSpeechDemo 管理麦克风/扬声器) ----
    JZSpeechDemo demo;

    // ---- 语音管线 (JZSpeechCore: Preprocess->VAD->KWS->ASR->LLM->TTS) ----
    JZSpeechCore core;

    // ---- ASR / TTS 节点来自 JZSpeechClient (远端服务) ----
    JZSpeechClient speech;
    core.setAsr(speech.getAsrInterface());
    core.setTts(speech.getTtsInterface());
    core.setVadEnabled(!novad);
    core.setKwsEnabled(!nokws);

    // TODO: 配置 LLM (JZAgent)
    // core.llm()->setSystemPrompt("你是一个语音助手...");
    // core.llm()->init(agentConfig);

    // ---- 接线: 采集 -> 管线 ; 管线合成音频 -> 播放 ----
    demo.setAudioInputCallback([&](const JZByteArray &pcm) {
        core.audioInput(pcm);
    });
    core.setAudioOutputSink([&](const JZByteArray &pcm) {
        demo.play(pcm);
    });

    connect(&speech.sigConnected, [&]() {
        jzDebug() << "[speech] ASR/TTS 服务已连接";
    });
    connect(&speech.sigError, [&]() {
        jzDebug() << "[speech] 服务错误, 退出";
        loop.quit();
    });

    if (!demo.open(inDev, outDev))
    {
        jzDebug() << "[speech] 打开音频设备失败";
        return -1;
    }
    if (!speech.connectServer(url))
    {
        jzDebug() << "[speech] 连接 ASR/TTS 服务失败";
        return -1;
    }

    // 回车退出
    JZConsoleInput console;
    connect(&console.sigReceived, [&](const std::string &) {
        jzDebug() << "[speech] 收到退出指令";
        demo.close();
        loop.quit();
    });
    console.setPrompt("按回车退出...");
    console.start();

    jzDebug() << "[speech] 语音助手 demo 运行中 (回车退出)";
    loop.exec();

    jzDebug() << "[speech] 已退出 | 采集 " << demo.capturedBytes()
              << " B, 播放 " << demo.playedBytes() << " B";
    return app.exec();
}

int main(int argc, char *argv[])
{
    return app_main(argc, argv);
}
