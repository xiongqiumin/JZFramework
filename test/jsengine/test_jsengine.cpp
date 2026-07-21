#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZTimer.h"
#include "JZCo.h"
#include "JZJSEngine.h"
#include "JZJSBind.h"

using namespace std;

static int g_fail = 0;

static void check(const string &name, bool ok)
{
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

// 注册到 JS 的示例类
struct MyMath
{
    int sub(int a, int b) { return a - b; }
    int mul(int a, int b) { return a * b; }
};

int main()
{
    JZApplication a;
    jzco_init();                     // 初始化 jzco 协程调度器(主线程)

    JZJSEngine engine;
    engine.init();

    // 日志 / 错误输出到控制台
    connect(&engine.sigLog, [](const string &s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string &s) { jzDebug() << "[js:error] " << s; });

    // ==================== 基础测试 ====================

    // 1. 基础表达式求值
    {
        auto v = engine.eval("1 + 2 * 3");
        check("eval basic", !engine.isException(v.get()) && engine.fromJSValue<int>(v.get()) == 7);
    }

    // 2. 字符串拼接
    {
        auto v = engine.eval("'hello ' + 'world'");
        check("eval string", engine.fromJSValue<string>(v.get()) == "hello world");
    }

    // 3. 注册同步 C++ 函数 (int, int) -> int
    engine.registFunction(engine.globalObject(), "add",
        jsbind::createFunction([](int a, int b) -> int { return a + b; }));
    {
        auto v = engine.eval("add(3, 4)");
        check("registFunction add", engine.fromJSValue<int>(v.get()) == 7);
    }

    // 4. 注册返回 string 的函数
    engine.registFunction(engine.globalObject(), "greet",
        jsbind::createFunction([](const string &name) -> string { return "hi " + name; }));
    {
        auto v = engine.eval("greet('js')");
        check("registFunction greet", engine.fromJSValue<string>(v.get()) == "hi js");
    }

    // 5. 注册类及成员函数
    jsbind::ClassBind<MyMath> bind("MyMath");
    bind.def("sub", &MyMath::sub);
    bind.def("mul", &MyMath::mul);
    engine.registClass(bind.classDefine());
    {
        auto v = engine.eval("var m = new MyMath(); m.sub(10, 3) + m.mul(2, 5);");
        check("registClass MyMath", engine.fromJSValue<int>(v.get()) == 17);
    }

    // 6. 在 JS 中使用 console.log 与已注册函数
    {
        auto v = engine.eval(
            "var r = add(1, 2);"
            "console.log('js compute: ' + r);"
            "r;");
        check("console + eval", engine.fromJSValue<int>(v.get()) == 3);
    }

    // 7. 异常可被检测
    {
        auto v = engine.eval("no_such_var.foo()");
        check("exception detect", engine.isException(v.get()));
        engine.deinit();
        engine.init();   // 重新初始化, 清除异常上下文状态
    }

    // 8. 重新初始化后仍能正常求值
    {
        auto v = engine.eval("2 * 21");
        check("reinit eval", engine.fromJSValue<int>(v.get()) == 42);
    }

    // ==================== 定时器 + 异步函数测试 (统一事件循环) ====================

    engine.start();      // 启动引擎定时器, 周期性执行 timer 与 promise 任务

    // 定时器结果
    int g_timer_timeout = -1;   // setTimeout 是否触发 (1/0)
    int g_timer_interval = -1;  // setInterval 触发次数
    bool g_timer_done = false;

    // 异步结果
    int g_async_result = -1;
    bool g_async_done = false;
    string g_async_err;
    bool g_async_err_done = false;

    // 定时器与异步都完成后才退出事件循环
    auto allDone = [&]() {
        return g_timer_done && (g_async_done || g_async_err_done);
    };

    engine.registFunction(engine.globalObject(), "reportTimer",
        jsbind::createFunction([&](int timeout_ok, int interval_count) -> void {
            g_timer_timeout = timeout_ok;
            g_timer_interval = interval_count;
            g_timer_done = true;
            if (allDone()) a.quit();
        }));
    engine.registFunction(engine.globalObject(), "reportResult",
        jsbind::createFunction([&](int v) -> void {
            g_async_result = v;
            g_async_done = true;
            if (allDone()) a.quit();
        }));
    engine.registFunction(engine.globalObject(), "reportError",
        jsbind::createFunction([&](const string &s) -> void {
            g_async_err = s;
            g_async_err_done = true;
            if (allDone()) a.quit();
        }));

    // 13. setTimeout 单次触发 + setInterval 多次触发并用 clearInterval 停止
    engine.eval(
        "(function() {"
        "  var timeout_ok = false;"
        "  setTimeout(function() { timeout_ok = true; }, 30);"
        "  var count = 0;"
        "  var id = setInterval(function() {"
        "    count = count + 1;"
        "    if (count >= 3) {"
        "      clearInterval(id);"                 // 达到次数后停止, 不应再触发
        "      reportTimer(timeout_ok ? 1 : 0, count);"
        "    }"
        "  }, 40);"
        "})();");

    // 9. 异步函数: jzco_sleep 模拟延时后 resolve (int,int)->int
    //    注意: 协程体内部会调用 setPromiseResolve -> JS_Call(深层递归),
    //    必须使用足够大的协程栈(默认 1024B 会栈溢出), 这里给 256KB
    engine.registAsyncFunction(engine.globalObject(), "asyncDelayAdd",
        jsbind::createFunction([&engine](int handle, int a, int b) -> void {
            jzco_spawn([&engine, handle, a, b]() {
                jzco_sleep(50);   // 模拟异步等待
                engine.setPromiseResolve(handle, { engine.toValuePtr(a + b) });
            }, 256 * 1024);
        }));

    // 10. 异步函数: jzco_submitAndWait 把重计算丢到线程池, 再 resolve
    engine.registAsyncFunction(engine.globalObject(), "asyncCompute",
        jsbind::createFunction([&engine](int handle, int n) -> void {
            jzco_spawn([&engine, handle, n]() {
                int sum = 0;
                jzco_submitAndWait([&sum, n]() {
                    for (int i = 1; i <= n; i++)
                        sum += i;
                });
                engine.setPromiseResolve(handle, { engine.toValuePtr(sum) });
            }, 256 * 1024);
        }));

    // 11. 异步函数: 主动 reject
    engine.registAsyncFunction(engine.globalObject(), "asyncReject",
        jsbind::createFunction([&engine](int handle) -> void {
            jzco_spawn([&engine, handle]() {
                jzco_sleep(10);
                engine.setPromiseReject(handle, "async error test");
            }, 256 * 1024);
        }));

    // 12. JS 中 await 串联多个异步 + 捕获 reject
    engine.eval(
        "(async function() {"
        "  var r1 = await asyncDelayAdd(3, 4);"        // 7
        "  var r2 = await asyncCompute(100);"          // 5050
        "  var total = r1 + r2;"                       // 5057
        "  console.log('async total: ' + total);"
        "  var caught = false;"
        "  try { await asyncReject(); }"
        "  catch (e) { caught = true; reportError(e.message); }"
        "  if (caught) reportResult(total);"
        "  else reportResult(-1);"
        "})();");

    // 统一事件循环: 定时器与异步全部完成, 或 5s 看门狗超时退出
    JZTimer watchdog;
    watchdog.setSingleShot(true);
    connect(&watchdog.sigTimeout, [&a]() {
            jzDebug() << "[watchdog] timeout";
            a.quit();
        });
    watchdog.start(5000);
    a.exec();

    check("setTimeout fired once", g_timer_done && g_timer_timeout == 1);
    check("setInterval fired 3 times then cleared", g_timer_done && g_timer_interval == 3);
    check("async delayAdd + compute", g_async_done && g_async_result == 5057);
    check("async reject caught", g_async_err_done && g_async_err == "async error test");

    engine.stop();
    engine.deinit();

    if (g_fail == 0)
        jzDebug() << "【OK】JSEngine 测试全部通过";
    else
        jzDebug() << "【FAIL】JSEngine 测试失败数: " << g_fail;

    return g_fail == 0 ? 0 : 1;
}
