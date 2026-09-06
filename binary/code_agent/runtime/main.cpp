// code_runtime - 运行"已编译好的 TS 工程产物"的隔离进程（无 GUI）。
//
// 用法:
//   code_runtime <projectRoot>
//      projectRoot  工程根目录（tsconfig.json / project.json 所在目录，
//                   同时作为引擎工作目录）
//
// 行为:
//   1) rt.init(projectRoot) 一步完成：设运行根 -> 读取 project.json 的 entry
//      拿到主入口（工程必须先"打包/发布"——已 tsc 编译并生成 project.json；
//      读不到或产物不存在直接报错退出，不回退默认入口）
//      -> 新建干净引擎会话 -> 以 ESM 模块方式加载并执行主入口（引擎自动解析
//      import 依赖，扩展名可省略，自动补 .js / index.js；入口具名导出绑定到全局）
//      -> 自动调用入口主函数 main()。
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
        "code_runtime - 运行已打包/发布好的 TS 工程产物（隔离进程，无 GUI）\n"
        "主入口读取工程根 project.json 的 entry；缺失或产物不存在时报错退出");

    string projectRoot;
    JZCommandLineArgument arg;
    arg.name = "projectRoot";
    arg.description =
        "工程根目录（tsconfig.json / project.json 所在目录，同时作为引擎工作目录）";
    arg.required = true;
    parser.addArgument(arg, &projectRoot);

    // 调试：--debug / -d 强制开启调试器；否则读自身"调试开关"资源（导出时写入，缺省 true）。
    // --debug-port 指定 HTTP 监听端口（默认 9222）。
    bool cmdDebug = false;
    parser.addOption("--debug,-d", &cmdDebug);
    int debugPort = 9222;
    parser.addOption("--debug-port", &debugPort);

    // 解析失败：parser 内部已打印 usage + 错误并 std::exit(1)
    //（JZCommandLineParser::reportError 的实现约定，控制不到 main）；
    // "-h/--help" 由 parser 打印完整帮助后 std::exit(0)。
    if (!parser.parse(argc, argv))
        return 1;   // 防御：真走到这里说明用法不合法

    string root = projectRoot;
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
        root.pop_back();

    JZDir dir(root);
    if (!dir.exists() || !JZFileInfo(root).isDir())
    {
        printf("[code_runtime] project root not found: %s\n", root.c_str());
        return 2;
    }
    string absRoot = dir.absolutePath();

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
        printf("%s\n", text.c_str());
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
        printf("%s\n", text.c_str());
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
    if (ok)
    {
        printf("[code_runtime] module run OK\n");
        fflush(stdout);
    }
    else
    {
        printf("[code_runtime] error while running project '%s':\n%s\n",
               absRoot.c_str(), rt.lastError().c_str());
        fflush(stdout);
        // 若引擎未在 sigError 阶段上报（如同步 eval 错误），这里手动触发一次
        if (!crashReported)
            onUnhandledException(rt.lastError());
    }

    if (crashExit)
        return 1;

    // 入口模块(以及自动调用的 main()) 的同步部分已经跑完。若引擎里还挂着未
    // 执行的异步任务（async main / 顶层 await 遗留的 promise、JS 的
    // setTimeout/setInterval 等），进程此刻退出会把它们丢掉 —— 这类任务只能靠
    // 事件循环驱动引擎的 10ms 定时器逐批泵完，所以有任务时要留在事件循环里等：
    //   - 没有待执行任务且未因异常保持存活：不必进事件循环，直接按 init 成败退出；
    //   - 还有任务，或因异常需保持存活（调试器已/将启动）：进 app.exec()，由守卫
    //     定时器在任务排空（hasPendingJob 变 false）后调 app.exit() 退出。
    //     （setInterval 之类常驻任务永不排空，进程会一直运行，由外部终止。）
    //   keepAlive（调试器已启动或因异常选择"开始调试"）时即使没有待执行任务也要
    //   留在事件循环里，让 HTTP 调试服务持续监听（由外部终止进程）。
    if (!keepAlive && !rt.hasPendingJob())
        return ok ? 0 : 1;

    JZTimer check_timer;
    connect(&check_timer.sigTimeout, [&]() {
        // keepAlive 被异常流程置 true 时（调试器已/将启动）不自动退出
        if (!keepAlive && !rt.hasPendingJob())
            app.exit(ok ? 0 : 1);
    });
    check_timer.start(100);

    return app.exec();
}
