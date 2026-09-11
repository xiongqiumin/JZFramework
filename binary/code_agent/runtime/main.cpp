// code_runtime - 运行"已编译好的 TS 工程产物"的隔离进程（无 GUI）。
//
// 用法:
//   code_runtime                    （不带参数）
//     运行根 = 当前工作目录（jzApp->workingDirPath()，等价 Qt 的 QDir::currentPath()），
//     即 main.js 所在的产物目录（如 <工程根>/build）。双击 exe / run.bat 时 cwd 就是
//     包所在目录；要以别的目录为运行根，由调用方设置子进程工作目录。
//
// 行为:
//   1) rt.init(root) 一步完成：设运行根 -> 以主入口 main.js 跑起来
//      （主入口固定为运行根下的 main.js；运行根下可读到 project.json 时，
//      里面的"原路径地址/编译时间"等补充信息会用于解析与展示）
//      -> 新建干净引擎会话 -> 以 ESM 模块方式加载并执行 main.js（引擎自动解析
//      import 依赖，扩展名可省略，自动补 .js / index.js；入口具名导出绑定到全局）。
//      找不到 main.js（工程尚未打包/发布）直接报错退出，不回退默认入口。
//   2) 模块 console.* 输出转发到本进程 stdout。
//   3) 出错时，JZEngineRuntime 内部把 JS 栈按 source map 翻译回 TS 源码位置，
//      打印后退出码返回 1；成功返回 0。
//
// JZJSEngine 的初始化/调用/栈翻译都封装在 JZEngineRuntime 里，这里只是命令行
// 外壳 + 输出。供 code_agent 及其测试以子进程方式"调用运行时"。
//
// 命令行解析走 JZCommandLineParser（本仓库标准工具，带 usage/help）。
//
// 注意: 本文件尽量不主动 include <windows.h>；但崩溃弹窗用到 JZWidget（JZDialog 间接
// 引入 windows.h），且本文件已通过 JZExeResource.h 引入过 windows.h，属既有情况。

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZDir.h"
#include "JZEngineRuntime.h"
#include "JZExeResource.h"
#include "JZFileInfo.h"
#include "JZPlatform.h"
#include "JZRuntimeDebugger.h"
#include "JZTimer.h"

#include "JZCrashReport.h"
#include "JZDialog.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZMessageBox.h"
#include "JZDebug.h"

using namespace std;

int main(int argc, char *argv[])
{
    JZApplication app;

    JZCommandLineParser parser;
    parser.setDescription(
        "code_runtime - 运行已打包/发布好的 TS 工程产物（隔离进程）\n"
        "主入口读取工程根 main.js 的 entry；缺失或产物不存在时报错退出\n"
        "支持 JS 创建 GUI（窗口/画布）；存在可见顶层窗口时不自动退出");

    JZCommandLineArgument arg;

    // 调试：--debug / -d 强制开启调试器；否则读自身"调试开关"资源（导出时写入，缺省 true）。
    // --debug-port 指定 HTTP 监听端口（默认 9222）。
    bool cmdDebug = false;
    parser.addOption("--debug,-d", &cmdDebug);
    int debugPort = 9222;
    parser.addOption("--debug-port", &debugPort);

    // 运行根：--root <dir> 显式指定；缺省为当前工作目录。
    string cmdRoot;
    parser.addOption("--root", &cmdRoot);

    if (!parser.parse(argc, argv))
        return 1;   // 防御：真走到这里说明用法不合法

    // 运行根：显式 --root 优先，否则用当前工作目录（双击 exe / run.bat 时 cwd 即包所在目录）。
    // 要以别的目录为运行根，也可由调用方设置子进程工作目录。
    string absRoot = cmdRoot.empty()
        ? jzApp->workingDirPath()
        : JZDir(cmdRoot).absolutePath();

    // 是否开启调试器：命令行 --debug 优先；否则读自身资源（导出时由 JZExeResource 写入）。
    bool debugMode = cmdDebug;
    if (!debugMode)
    {
        string derr;
        JZExeResource::getDebugFlag(JZPlatform::applicationFilePath(), debugMode, derr);
    }

    // 调试模式下启动 JZRuntimeDebugger（基于 JZMcpServer 的 HTTP 监听，端点 /mcp）。
    // 用堆对象使其生命周期覆盖整个进程（main 在 app.exec() 返回前不析构）。
    JZRuntimeDebugger* debugger = nullptr;
    if (debugMode)
    {
        debugger = new JZRuntimeDebugger();
        debugger->setEngine(nullptr); // 预留：控制 JS 用，暂未接入引擎
        if (debugger->start(debugPort))
        {
            printf("[code_runtime] debug HTTP listening: http://127.0.0.1:%d/mcp\n", debugPort);
            fflush(stdout);
        }
        else
        {
            printf("[code_runtime] failed to start debug HTTP server\n");
            fflush(stdout);
            delete debugger;
            debugger = nullptr;
        }
    }

    JZEngineRuntime rt;
    rt.setLogHandler([](const string &text) {
        jzDebug("%s\n", text.c_str());
        fflush(stdout);
    });

    // ---- 未处理 JS 异常：生成 crash report + 弹窗；debug 版可"开始调试"启动服务 ----
    bool crashReported = false;   // 保证未处理异常只处理一次
    bool crashExit = false;       // 用户选择退出
    bool keepAlive = (debugger != nullptr);   // 异常流程置 true 时保持进程存活（供调试）

    auto reportCrash = [&](const string &tsError) -> string {
        JZCrashReport r;
        r.platform = "windows";
        r.signal = 0;
        r.msg = tsError;
        r.stack = tsError;
        string text = JZCrashReportManager::instance()->makeReport(r);
        jzDebug("%s\n", text.c_str());
        fflush(stdout);
        return text;
    };

    auto onUnhandledException = [&](const string &tsError) {
        if (crashReported)
            return;
        crashReported = true;

        string shown = tsError;
        if (shown.size() > 1500)
            shown = shown.substr(0, 1500) + "\n...(已截断)";
        reportCrash(tsError);

        if (debugMode)
        {
            // 调试版：提示错误，可"开始调试"启动服务（若尚未启动），随后保持进程存活
            JZDialog dlg(nullptr);
            JZLabel *label = new JZLabel(&dlg);
            label->setText("[运行时错误]\n" + shown);
            label->move(20, 20);
            label->resize(440, 200);
            JZPushButton *startBtn = new JZPushButton(&dlg);
            startBtn->setText("开始调试");
            startBtn->move(70, 240);
            startBtn->resize(120, 34);
            JZPushButton *exitBtn = new JZPushButton(&dlg);
            exitBtn->setText("退出");
            exitBtn->move(270, 240);
            exitBtn->resize(120, 34);
            connect(&startBtn->sigClicked, &dlg, &JZDialog::accept);
            connect(&exitBtn->sigClicked, &dlg, &JZDialog::reject);
            if (dlg.exec() == JZDialog::Accepted)
            {
                if (!debugger)
                {
                    debugger = new JZRuntimeDebugger();
                    debugger->setEngine(nullptr);
                    if (debugger->start(debugPort))
                        printf("[code_runtime] debug HTTP listening: http://127.0.0.1:%d/mcp\n", debugPort);
                    else
                        printf("[code_runtime] failed to start debug HTTP server\n");
                    fflush(stdout);
                }
                keepAlive = true;
            }
            else
            {
                crashExit = true;
                app.exit(1);
            }
        }
        else
        {
            // 非调试版：弹窗提示后退出
            if(jzApp->activeWindow())
            {
                JZMessageBox::information(jzApp->activeWindow(), "运行时错误", shown)->exec();
                crashExit = true;
                app.exit(1);
            }
            else
            {
                jzError() << "运行时错误" << shown;
                crashExit = true;
                app.exit(1);
            }                
        }
    };
    rt.setUnhandledExceptionHandler(onUnhandledException);

    bool ok = rt.init(absRoot);
    if (!ok)
    {
        jzDebug() << rt.lastError().c_str();
        return 1;
    }

    // 存在可见顶层窗口（如 JS 建的五子棋画布窗口）时也要留在事件循环里，等用户关窗。
    auto hasVisibleTopWindow = []() -> bool {
        for (auto* w : jzApp->topLevelWindows())
            if (w->isVisible() && w->nativeHandle())
                return true;
        return false;
    };

    if (!keepAlive && !rt.hasPendingJob() && !hasVisibleTopWindow())
        return 0;

    JZTimer check_timer;
    connect(&check_timer.sigTimeout, [&]() {
        // keepAlive 被异常流程置 true 时（调试器已/将启动）不自动退出
        if (keepAlive)
            return;
        // 有可见顶层窗口（如 JS 建的五子棋画布窗口）时保持运行，等用户关窗再退出
        if (hasVisibleTopWindow())
            return;
        if (!rt.hasPendingJob())
            app.quit();
    });
    check_timer.start(100);

    return app.exec();
}
