// JZCodeAgent —— 基于 JZAgent + JZWidget 的本地代码助手（桌面版）
//
// 启动后是一个原生窗口：上方输出框显示任务执行过程，下方输入框提交任务。
// 大模型通过 OpenAI 兼容接口调用（DashScope / vLLM / Ollama 等均可），
// 模型、Token、工作区等参数在“设置”里填写，并持久化到 exe 目录的 code_agent.json。

#include "JZApplication.h"
#include "MainWindow.h"

using namespace std;

int main(int argc, char* argv[])
{
    JZApplication app;

    MainWindow w;
    w.show();

    return app.exec();
}
