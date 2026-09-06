#include <chrono>
#include <string>

#include "JZDebug.h"
#include "JZApplication.h"
#include "JZEventLoop.h"
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
    JZEventLoop loop;                // 事件循环: 可重入, 每次 exec() 都重新运行
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

    engine.start();      // 启动引擎调度(running 态; 事件驱动下无任务时不上定时器)

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
            if (allDone()) loop.quit();
        }));
    engine.registFunction(engine.globalObject(), "reportResult",
        jsbind::createFunction([&](int v) -> void {
            g_async_result = v;
            g_async_done = true;
            if (allDone()) loop.quit();
        }));
    engine.registFunction(engine.globalObject(), "reportError",
        jsbind::createFunction([&](const string &s) -> void {
            g_async_err = s;
            g_async_err_done = true;
            if (allDone()) loop.quit();
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
    connect(&watchdog.sigTimeout, [&loop]() {
            jzDebug() << "[watchdog] timeout";
            loop.quit();
        });
    watchdog.start(5000);
    loop.exec();

    check("setTimeout fired once", g_timer_done && g_timer_timeout == 1);
    check("setInterval fired 3 times then cleared", g_timer_done && g_timer_interval == 3);
    check("async delayAdd + compute", g_async_done && g_async_result == 5057);
    check("async reject caught", g_async_err_done && g_async_err == "async error test");

    // 宿主定时器存在较大固定延迟, watchdog 不再需要跨用例存活, 及时停掉避免干扰后续 exec
    watchdog.stop();

    // ==================== 补充用例: 定时器取消 / 自然多次 / 异常态 ====================

    // 引擎一旦进入 Engine_error, 主动退出事件循环, 使异常态用例可以确定性结束
    connect(&engine.sigStatus, [&loop](int s) { if (s == Engine_error) loop.quit(); });

    // 14. clearTimeout 取消尚未触发的 setTimeout (回归 #1: setInterval/setTimeout 必须返回真实 id)
    {
        bool timeout_fired = false;
        engine.registFunction(engine.globalObject(), "onTimeoutFired",
            jsbind::createFunction([&](int v) -> void {
                timeout_fired = (v == 1);
                loop.quit();
            }));
        engine.eval(
            "(function() {"
            "  var id = setTimeout(function() { onTimeoutFired(1); }, 50);"
            "  clearTimeout(id);"                 // 立即取消, 不应再触发
            "})();");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(300);        // 若未被取消, 50ms 就会触发并提前退出
        loop.exec();

        check("clearTimeout cancels setTimeout", !timeout_fired);
    }

    // 15. setInterval 自然多次触发 (验证 reschedule 路径, 不依赖 clearInterval 的取消时机)
    {
        int interval_count = 0;
        engine.registFunction(engine.globalObject(), "onIntervalTick",
            jsbind::createFunction([&](int c) -> void {
                interval_count = c;
                if (c >= 5)
                    loop.quit();   // 只在达到目标次数后退出; JS 侧已 clearInterval(id) 停止
            }));
        engine.eval(
            "(function() {"
            "  var count = 0;"
            "  var id = setInterval(function() {"
            "    count = count + 1;"
            "    if (count >= 5) clearInterval(id);"   // 自然达到次数后自行停止, 不再泄漏
            "    onIntervalTick(count);"
            "  }, 30);"
            "})();");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(2000);
        loop.exec();

        check("setInterval fires repeatedly (reschedule)", interval_count >= 5);
    }

    // 16. 定时器回调抛异常 -> 引擎进入 Engine_error 并停止定时器
    {
        engine.eval(
            "(function() {"
            "  setTimeout(function() { throw new Error('timer boom'); }, 30);"
            "})();");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(2000);
        loop.exec();

        check("timer callback exception -> Engine_error", engine.status() == Engine_error);
    }

    // 17. 未处理的 Promise 拒绝 -> 引擎进入 Engine_error (重新 init 以恢复 running 态)
    {
        engine.deinit();
        engine.init();
        engine.start();

        engine.registAsyncFunction(engine.globalObject(), "asyncReject2",
            jsbind::createFunction([&engine](int handle) -> void {
                jzco_spawn([&engine, handle]() {
                    jzco_sleep(10);
                    engine.setPromiseReject(handle, "unhandled reject");
                }, 256 * 1024);
            }));

        engine.eval("(async function() { await asyncReject2(); })();");  // 不捕获 -> 未处理拒绝

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(2000);
        loop.exec();

        check("unhandled promise rejection -> Engine_error", engine.status() == Engine_error);
    }

    // ==================== 事件驱动专项 ====================
    // 引擎驱动已从"常驻 10ms 周期定时器"改为:
    //  - JS 定时任务: 按最近到期挂一次性 timer(scheduleNextTimer);
    //  - C++ promise 结算 / 外部入口: wakeUp() 投递 JZEngine_WakeUp 唤醒 execOnce;
    //  - 完全无任务时空闲停表。
    // 上一组用例结束时引擎停在 Engine_error, 先恢复干净会话。

    engine.stop();          // error -> none
    engine.deinit();
    engine.init();
    engine.start();

    // 18. setTimeout(f, 0) 允许且尽快触发(一次性任务挂最近到期)
    {
        bool zero_fired = false;
        engine.registFunction(engine.globalObject(), "onZeroFired",
            jsbind::createFunction([&](int v) -> void {
                zero_fired = (v == 1);
                loop.quit();
            }));
        engine.eval("setTimeout(function() { onZeroFired(1); }, 0);");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(1000);
        loop.exec();
        check("setTimeout(0) fires next round", zero_fired);
    }

    // 19. eval 里产生的 promise 微任务(没有任何 JS timer)也要被 wakeUp 泵掉
    {
        int got = -1;
        engine.registFunction(engine.globalObject(), "onMicroValue",
            jsbind::createFunction([&](int v) -> void {
                got = v;
                loop.quit();
            }));
        engine.eval("Promise.resolve(42).then(function(v) { onMicroValue(v); });");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(1000);
        loop.exec();
        check("promise microtask pumped by wakeUp", got == 42);
    }

    // 20. 最近到期优先: 后注册的更短任务应先触发(验证调度器会重挂到更早到期时刻)
    {
        string order;
        engine.registFunction(engine.globalObject(), "onFired",
            jsbind::createFunction([&](const string &name) -> void {
                if (!order.empty())
                    order += ",";
                order += name;
                if (name == "far")
                    loop.quit();
            }));
        engine.eval(
            "(function() {"
            "  setTimeout(function() { onFired('far'); }, 150);"
            "  setTimeout(function() { onFired('near'); }, 40);"
            "})();");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(1500);
        loop.exec();
        check("nearest timer fires first", order == "near,far");
    }

    // 21. 空闲停表后再次注入任务仍被驱动(不再依赖常驻周期 tick)
    {
        int got = -1;
        engine.registFunction(engine.globalObject(), "onStage",
            jsbind::createFunction([&](int stage) -> void {
                if (stage == 1)
                {
                    // 第一个一次性任务(30ms)已完成、引擎空闲; 这里重新注入一个任务,
                    // 应由 setInterval -> scheduleNextTimer 挂起新的一次性 timer
                    engine.eval("setTimeout(function() { onStage(2); }, 60);");
                }
                else if (stage == 2)
                {
                    got = stage;
                    loop.quit();
                }
            }));
        engine.eval("setTimeout(function() { onStage(1); }, 30);");

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(1500);
        loop.exec();
        check("new task after idle is still driven", got == 2);
    }

    // 22. 异步 loopback 驱动频率: C++ callAsyncFunction(jsTick) -> jsTick await cppTick(C++ 异步)
    //     -> C++ resolve -> jsTick 完成触发 sigAsyncResult -> C++ 立即再发起下一圈, 持续 5s,
    //     统计每秒引擎被驱动(execOnce 实际泵一轮)次数 与 每秒 loopback 圈数(cppTick 被回调次数)。
    //     注意: 宿主短 timer 有 ~1s 固定延迟, 本链不依赖任何 timer(纯 wakeUp 事件驱动)。
    {
        long long cpp_calls = 0;              // C++ 异步函数被 JS 回调次数 = loopback 圈数
        bool chain_done = false;
        auto t0 = std::chrono::steady_clock::now();

        engine.registAsyncFunction(engine.globalObject(), "cppTick",
            jsbind::createFunction([&](int handle) -> void {
                ++cpp_calls;
                engine.setPromiseResolve(handle, { engine.toValuePtr(1) });
            }));

        // JS 侧异步: await C++ 异步函数 cppTick 后返回 -> promise resolve -> sigAsyncResult
        engine.eval("async function jsTick() { await cppTick(); }");

        connect(&engine.sigAsyncResult, [&](const JZAsyncResult&) {
            if (chain_done)
                return;
            using namespace std::chrono;
            if (duration_cast<milliseconds>(steady_clock::now() - t0).count() >= 5000)
            {
                chain_done = true;   // 不再续跑, 链自然停止
                loop.quit();
            }
            else
            {
                engine.callAsyncFunction("jsTick", {});   // 立即发起下一圈
            }
        });

        engine.resetExecOnceCount();
        engine.callAsyncFunction("jsTick", {});   // 起跑第一圈

        JZTimer wd;    // 兜底(正常由 5s 到点退出)
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&loop]() { loop.quit(); });
        wd.start(8000);
        loop.exec();

        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0;
        if (secs < 0.001)
            secs = 0.001;
        unsigned long long exec_delta = engine.execOnceCount();
        jzDebug() << "[loopback] 持续 " << secs << "s:"
                  << " loopback 圈数=" << cpp_calls
                  << " (" << (long long)(cpp_calls / secs) << " 圈/s, 每圈=C++ 异步被回调一次)"
                  << " 引擎宏任务泵(execOnce)=" << exec_delta << " 次"
                  << " (微任务在单轮 pump 内自再生, 空闲不唤醒)";
        check("async loopback keeps driving", cpp_calls >= 100 && exec_delta >= 1);
    }

    // 23. 吞吐拆解(参照): 纯 JS 循环 / JS->C++ 同步调用 的原始量级,
    //     用于对比 async loopback(用例22, ~8400 圈/s)的每圈常数开销构成
    {
        engine.registFunction(engine.globalObject(), "jzNop",
            jsbind::createFunction([]() -> void {}));
        engine.registFunction(engine.globalObject(), "jzAdd",
            jsbind::createFunction([](int a, int b) -> int { return a + b; }));

        auto t0 = std::chrono::steady_clock::now();
        engine.eval(
            "function bench(tag, n, f) {"
            "  var s = Date.now();"
            "  if (!f) { for (var i = 0; i < n; i++) {} }"
            "  else { for (var i = 0; i < n; i++) f(); }"
            "  var e = Date.now();"
            "  console.log('[bench] ' + tag + ': ' + n + ' 次 / ' + (e - s) + 'ms = ' +"
            "    Math.round(n * 1000 / (e - s || 1)) + ' 次/s');"
            "}"
            "bench('JS 空循环', 3000000, null);"
            "bench('JS 空函数调用', 1000000, function(){});"
            "bench('JS->C++ 空函数 jzNop', 500000, function(){ jzNop(); });"
            "bench('JS->C++ 加法 jzAdd(1,2)', 300000, function(){ jzAdd(1, 2); });");
        auto t1 = std::chrono::steady_clock::now();
        jzDebug() << "[bench] eval 总耗时 "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << "ms";
    }

    engine.stop();
    engine.deinit();

    if (g_fail == 0)
        jzDebug() << "【OK】JSEngine 测试全部通过";
    else
        jzDebug() << "【FAIL】JSEngine 测试失败数: " << g_fail;

    return g_fail == 0 ? 0 : 1;
}
