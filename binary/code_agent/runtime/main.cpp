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
// 注意: 本文件不要 include <windows.h>。

#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCommandLineParser.h"
#include "JZDir.h"
#include "JZEngineRuntime.h"
#include "JZFileInfo.h"
#include "JZTimer.h"

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

    JZEngineRuntime rt;
    rt.setLogHandler([](const string &text) {
        printf("%s\n", text.c_str());
        fflush(stdout);
    });

    bool ok = rt.init(absRoot);
    if (ok)
    {
        printf("[code_runtime] module run OK\n");
        fflush(stdout);
    }
    else
    {
        // 运行失败：错误已翻译回 TS 源码位置
        printf("[code_runtime] error while running project '%s':\n%s\n",
               absRoot.c_str(), rt.lastError().c_str());
        fflush(stdout);
    }

    // 入口模块(以及自动调用的 main()) 的同步部分已经跑完。若引擎里还挂着未
    // 执行的异步任务（async main / 顶层 await 遗留的 promise、JS 的
    // setTimeout/setInterval 等），进程此刻退出会把它们丢掉 —— 这类任务只能靠
    // 事件循环驱动引擎的 10ms 定时器逐批泵完，所以有任务时要留在事件循环里等：
    //   - 没有待执行任务：不必进事件循环，直接按 init 成败退出；
    //   - 还有任务：进 app.exec()，由守卫定时器在任务排空（hasPendingJob 变
    //     false，即异步工作全部跑完）后调 app.exit() 退出。
    //     （setInterval 之类常驻任务永不排空，进程会一直运行，由外部终止。）
    if (!rt.hasPendingJob())
        return ok ? 0 : 1;

    JZTimer check_timer;
    connect(&check_timer.sigTimeout, [&]() {
        if (!rt.hasPendingJob())
            app.exit(ok ? 0 : 1);
    });
    check_timer.start(100);

    return app.exec();
}
