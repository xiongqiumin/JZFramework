// JZAgent 多模态(图片)测试:
//   第 1 轮: 带一张 64x64 纯红色 PNG(base64) 问颜色  -> 应回答"红"
//   第 2 轮: 不带图, 只问形状                        -> 仍能回答, 说明历史里的图片被正确重发
//
// 用法: test_image.exe [--modelUrl ...] [--modelName ...]

#include <string>

#include "JZAgent.h"
#include "JZApplication.h"
#include "JZByteArray.h"
#include "JZDebug.h"
#include "JZTimer.h"
#include "json/json.h"

using namespace std;

static const char *kDefaultModelUrl  = "http://172.17.160.147:8000/v1/chat/completions";
static const char *kDefaultModelName = "/workspace/RedHatAI/Qwen3.6-35B-A3B-NVFP4";

// 64x64 纯红色 PNG 的 base64
static const char *kRedPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAAAb0lEQVR4nO3PAQkAAAyEwO9feoshgnABdLep8QUNyPEFDcjx"
    "BQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxB"
    "Q3IPanc8OLDQitxAAAAAElFTkSuQmCC";

int main(int argc, char *argv[])
{
    string model_url = kDefaultModelUrl;
    string model_name = kDefaultModelName;
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg == "--modelUrl" && i + 1 < argc)  model_url = argv[++i];
        else if (arg == "--modelName" && i + 1 < argc) model_name = argv[++i];
    }

    JZApplication app;

    JZAgentConfig cfg;
    cfg.modelUrl = model_url;
    cfg.modelName = model_name;
    cfg.modelToken = "EMPTY";
    cfg.maxRounds = 4;
    cfg.netTimeout = 180000;
    cfg.maxToken = 150000;
    cfg.tokenStrategy = JZAgentConfig::TokenCompress;

    Json::Value extra(Json::objectValue);
    extra["max_tokens"] = 512;
    Json::Value tpl(Json::objectValue);
    tpl["enable_thinking"] = false;
    extra["chat_template_kwargs"] = tpl;
    cfg.extraBody = extra;

    JZAgent *agent = new JZAgent(&app);
    agent->init(cfg);
    agent->setSystemPrompt("你是一个助手, 看图后用简短的中文回答, 不要解释。");

    string reply;
    int round = 0;
    int result = 1;

    auto quit = [&]()
    {
        agent->blockSignals(true);
        app.quit();
    };

    connect(&agent->sigReply, [&reply](const string &text)
    {
        reply += text;
    });
    connect(&agent->sigError, [&](const string &err)
    {
        jzError() << "错误:" << err;
        result = 1;
        quit();
    });
    connect(&agent->sigFinished, [&]()
    {
        round++;
        jzInfo() << "第 " << round << " 轮回复:" << reply;

        if (round == 1)
        {
            bool pass = reply.find("红") != string::npos;
            if (!pass)
            {
                jzError() << "第 1 轮没有认出红色";
                result = 1;
                quit();
                return;
            }

            // 第 2 轮不再带图, 靠历史里保存的 data URL
            reply.clear();
            agent->request("它是什么形状? 只回答形状。");
            return;
        }

        result = (reply.find("方") != string::npos) ? 0 : 1;
        if (result != 0)
            jzError() << "第 2 轮没答出形状, 历史里的图片可能没生效";
        quit();
    });

    JZTimer::singleShot(0, [&]()
    {
        jzInfo() << "第 1 轮: 发送带图消息(base64)";
        string b64 = kRedPngBase64;
        agent->request("这张图是什么颜色? 只回答颜色。",
                       JZByteArray(b64.data(), (int)b64.size()));
    });

    JZTimer::singleShot(180000, [&]()
    {
        jzError() << "超时";
        result = 1;
        quit();
    });

    app.exec();
    jzInfo() << (result == 0 ? "测试通过" : "测试失败");
    return result;
}
