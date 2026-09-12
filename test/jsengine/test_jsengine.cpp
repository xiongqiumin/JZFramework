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

// 构造/析构计数被测类(指针 & 引用调用测试)
// 静态计数用于在 JS 对象以 值/const 值/引用/const 引用/指针/const 指针 六种方式
// 传入 C++ 时, 验证拷贝构造 / 析构次数是否符合预期(参考 C:\Users\xiong\Desktop\jz_test\test_net)。
struct AAA
{
    static int s_defaultCtor;
    static int s_copyCtor;
    static int s_moveCtor;
    static int s_copyAssign;
    static int s_moveAssign;
    static int s_dtor;

    AAA() { ++s_defaultCtor; }
    AAA(const AAA&) { ++s_copyCtor; }
    AAA(AAA&&) noexcept { ++s_moveCtor; }
    AAA& operator=(const AAA&) { ++s_copyAssign; return *this; }
    AAA& operator=(AAA&&) noexcept { ++s_moveAssign; return *this; }
    ~AAA() { ++s_dtor; }

    static void resetStats()
    {
        s_defaultCtor = s_copyCtor = s_moveCtor = s_copyAssign = s_moveAssign = s_dtor = 0;
    }
};
int AAA::s_defaultCtor = 0;
int AAA::s_copyCtor = 0;
int AAA::s_moveCtor = 0;
int AAA::s_copyAssign = 0;
int AAA::s_moveAssign = 0;
int AAA::s_dtor = 0;

// ClassPointer 指针包装测试对象(记录析构次数, 验证 JS 拥有时 GC 释放底层对象)
struct PP
{
    static int s_dtor;
    int v = 0;
    ~PP() { ++s_dtor; }
};
int PP::s_dtor = 0;

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

    // ==================== 指针 / 引用调用: 构造析构计数测试 ====================
    // 验证 JS 类实例以 值/const 值/引用/const 引用/指针/const 指针 六种方式传入 C++ 时,
    // 拷贝构造 / 析构次数是否符合预期(参考 jz_test/test_net 的 transFunc 计数用例)。
    {
        // 独立干净会话: 重新 init 以隔离之前注册的 MyMath 等, 结束后再次 init 恢复后续用例所需状态
        engine.deinit();
        engine.init();

        // 注册被测类 + 计数读取/重置 + 六种形参的接收函数
        jsbind::ClassBind<AAA> bind("AAA");
        engine.registClass(bind.classDefine());

        engine.registFunction(engine.globalObject(), "takeValue",
            jsbind::createFunction([](AAA) -> void {}));
        engine.registFunction(engine.globalObject(), "takeConstValue",
            jsbind::createFunction([](const AAA) -> void {}));
        engine.registFunction(engine.globalObject(), "takeRef",
            jsbind::createFunction([](AAA&) -> void {}));
        engine.registFunction(engine.globalObject(), "takeConstRef",
            jsbind::createFunction([](const AAA&) -> void {}));
        // 指针形参按规范统一用 ClassPointer<Class> 包装(不支持直接传入裸指针)
        engine.registFunction(engine.globalObject(), "takePtr",
            jsbind::createFunction([](jsbind::ClassPointer<AAA> p) -> void { (void)p.get(); }));
        engine.registFunction(engine.globalObject(), "takeConstPtr",
            jsbind::createFunction([](const jsbind::ClassPointer<AAA> p) -> void { (void)p.get(); }));
        
        // 单次调用统计: 重置 -> 调用 -> 读出拷贝/析构次数;
        // 持久实例 gInst 在本次调用中不会被析构, 故 dtor 只反映本次形参的拷贝析构
        auto checkCall = [&](const string& name, const string& js_func, bool isPtr, int expCopy, int expDtor) {
            AAA a;
            JZJSValuePtr js_a = engine.toValuePtr(a);
            JZJSValuePtr js_a_ptr = engine.toValuePtr(&a);

            AAA::resetStats();
            if (isPtr)
                engine.callFunction(js_func, { js_a_ptr });
            else
                engine.callFunction(js_func, { js_a });
            int copy = AAA::s_copyCtor;
            int dtor = AAA::s_dtor;
            check(name, copy == expCopy && dtor == expDtor);
        };

        checkCall("takeValue", "takeValue", false, 1, 1);
        checkCall("takeConstValue", "takeConstValue", false, 1, 1);
        checkCall("takeRef", "takeRef", false, 0, 0);
        checkCall("takeConstRef", "takeConstRef", false, 0, 0);
        checkCall("takePtr", "takePtr", true, 0, 0);
        checkCall("takeConstPtr", "takeConstPtr", true, 0, 0);

        // ==================== ClassPointer: 指针包装 / clear 触发 JS 异常 ====================
        // 验证: C 函数返回裸指针与 ClassPointer 都能包装成 JS 对象; 以 ClassPointer& 形参接收;
        //       C++ 侧 clear() 后, JS 再引用该对象即在 check type 阶段抛出 JS 异常。
        {
            engine.deinit();
            engine.init();

            // 注册 PP 类(值类与 ClassPointer<PP> 统一为同一个 JS 类), 并注册字段 v 以便 JS 侧读写转发到底层对象
            jsbind::ClassBind<PP> bindPP("PP");
            bindPP.defProperty("v", &PP::v);
            engine.registClass(bindPP.classDefine());

            static PP s_ppOwned;   // C++ 持有, 用于验证 cppOwner 语义

            // 返回裸指针: 包装成 ClassPointer(JS 拥有)
            engine.registFunction(engine.globalObject(), "makePRaw",
                jsbind::createFunction([]() -> PP* { return new PP(); }));
            // 返回 ClassPointer(C++ 拥有): JS 仅持句柄, 可 clear()
            engine.registFunction(engine.globalObject(), "makePOwned",
                jsbind::createFunction([]() -> jsbind::ClassPointer<PP> {
                    return jsbind::ClassPointer<PP>(&s_ppOwned, true);
                }));
            // 接收 ClassPointer(按引用: fromJSValue 返回的是 JS 对象内部 wrapper 的引用,
            // 因此 clear() 直接作用于 JS 持有的对象; 已 clear 时再次引用即在 check type 阶段抛 JS 异常)
            engine.registFunction(engine.globalObject(), "useP",
                jsbind::createFunction([](jsbind::ClassPointer<PP>& p) -> int {
                    return p.get() ? p.get()->v : -1;
                }));
            engine.registFunction(engine.globalObject(), "clearP",
                jsbind::createFunction([](jsbind::ClassPointer<PP>& p) -> void {
                    p.clear();
                }));

            // (a) 裸指针返回 -> JS 对象可用, 读取字段
            {
                auto v = engine.eval(
                    "var p = makePRaw();"
                    "useP(p);");
                check("ClassPointer raw ptr return + read field", engine.fromJSValue<int>(v.get()) == 0);
            }

            // (b) ClassPointer(C++ 拥有) 返回 -> 可读
            {
                auto v = engine.eval(
                    "var p = makePOwned();"
                    "useP(p);");
                check("ClassPointer cpp-owned return + read field", engine.fromJSValue<int>(v.get()) == 0);
            }

            // (c) clear 后再次引用 -> JS 异常
            {
                auto v = engine.eval(
                    "var p = makePOwned();"
                    "clearP(p);"
                    "useP(p);");
                check("ClassPointer clear then use -> JS exception", engine.isException(v.get()));
            }

            engine.deinit();
            engine.init();
        }

        engine.deinit();
        engine.init();   // 恢复后续用例所需的干净会话
    }

    // ==================== 各类 fromJSValue / toJSValue 转换全验证 ====================
    // 覆盖: 标量/字符串/Json/vector/类对象(值)/ClassPointer/裸指针(Class*)/原始JSValue 双向,
    // 以及 fromJSValue<Class*> 对 null/undefined/非指针/已clear 的处理。
    {
        engine.deinit();
        engine.init();

        jsbind::ClassBind<PP> bindPP("PP");
        bindPP.defProperty("v", &PP::v);
        engine.registClass(bindPP.classDefine());
        jsbind::ClassBind<AAA> bindAAA("AAA");
        engine.registClass(bindAAA.classDefine());

        auto toV = [&](auto&& t) { return engine.makeValuePtr(engine.toJSValue(t)); };

        // 1) 标量 / 字符串 / Json 双向(C++ 直接转换)
        check("conv bool", engine.fromJSValue<bool>(toV(true).get()) == true);
        check("conv int", engine.fromJSValue<int>(toV(7).get()) == 7);
        check("conv double", engine.fromJSValue<double>(toV(1.5).get()) == 1.5);
        check("conv string", engine.fromJSValue<string>(toV(string("hi")).get()) == "hi");
        {
            Json::Value j; j["a"] = 1; j["b"] = "x";
            Json::Value j2 = engine.fromJSValue<Json::Value>(toV(j).get());
            check("conv Json", j2["a"].asInt() == 1 && j2["b"].asString() == "x");
        }

        // 2) vector 双向
        {
            vector<int> vi = { 1, 2, 3 };
            check("conv vector<int>", engine.fromJSValue<vector<int>>(toV(vi).get()) == vi);
        }
        {
            vector<string> vs = { "a", "b" };
            check("conv vector<string>", engine.fromJSValue<vector<string>>(toV(vs).get()) == vs);
        }
        {
            PP p; p.v = 7;
            vector<PP> vp = { p };
            vector<PP> vp2 = engine.fromJSValue<vector<PP>>(toV(vp).get());
            check("conv vector<PP>", vp2.size() == 1 && vp2[0].v == 7);
        }

        // 3) 类对象(值) 双向: makePP 构造带字段, readP 读取字段
        engine.registFunction(engine.globalObject(), "makePP",
            jsbind::createFunction([]() -> PP { PP p; p.v = 42; return p; }));
        engine.registFunction(engine.globalObject(), "readP",
            jsbind::createFunction([](PP p) -> int { return p.v; }));
        {
            auto v = engine.eval("readP(makePP());");
            check("conv class value", engine.fromJSValue<int>(v.get()) == 42);
        }

        // 4) ClassPointer 双向(cpp 拥有, JS 仅句柄): 构造/读取/回传
        engine.registFunction(engine.globalObject(), "makeCppP",
            jsbind::createFunction([]() -> jsbind::ClassPointer<PP> {
                return jsbind::ClassPointer<PP>(new PP(), true);
            }));
        engine.registFunction(engine.globalObject(), "readCppP",
            jsbind::createFunction([](jsbind::ClassPointer<PP> p) -> int { return p.get() ? p.get()->v : -1; }));
        engine.registFunction(engine.globalObject(), "echoCppP",
            jsbind::createFunction([](jsbind::ClassPointer<PP> p) -> jsbind::ClassPointer<PP> { return p; }));
        {
            auto v = engine.eval("var p = makeCppP(); p.v = 9; readCppP(p);");
            check("conv ClassPointer read", engine.fromJSValue<int>(v.get()) == 9);
        }
        {
            // 按值返回的 ClassPointer 会复制 shared_ptr(指向同一底层对象), 不再是同一 JS 身份;
            // 验证回传后仍可读写底层对象即可(身份比较 === 在按值返回语义下不成立)。
            auto v = engine.eval("var p = makeCppP(); p.v = 9; readCppP(echoCppP(p));");
            check("conv ClassPointer echo", engine.fromJSValue<int>(v.get()) == 9);
        }

        // 5) 裸指针(Class*) 提取: fromJSValue<Class*> 直接取 ClassPointer 内裸指针;
        //    null/undefined 映射为空指针, 非指针类型报错, 已 clear 报错。
        {
            PP obj; obj.v = 5;
            JZJSValuePtr jsPtr = engine.toValuePtr(&obj);   // ClassPointer<PP>(cppOwner=true)
            PP* raw = engine.fromJSValue<PP*>(jsPtr.get());
            check("conv fromJSValue<PP*>", raw == &obj && raw->v == 5);

            // null / undefined -> nullptr
            JSValue nullv = JS_NULL;
            check("conv fromJSValue<PP*> null", engine.fromJSValue<PP*>(nullv) == nullptr);
            JSValue undefv = JS_UNDEFINED;
            check("conv fromJSValue<PP*> undefined", engine.fromJSValue<PP*>(undefv) == nullptr);
        }

        // 6) 原始 JSValue 双向
        engine.registFunction(engine.globalObject(), "echoJs",
            jsbind::createFunction([](JSValue v) -> JSValue { return v; }));
        {
            auto v = engine.eval("echoJs(123) === 123;");
            check("conv JSValue", engine.fromJSValue<bool>(v.get()) == true);
        }

        // 7) 异常路径(会在 ctx 留下 pending 异常, 放在最后, 块尾 reinit 重置):
        //    非指针 JSValue -> 抛异常; 已 clear 的 ClassPointer -> 抛异常。
        {
            bool threwNonPtr = false;
            try { (void)engine.fromJSValue<PP*>(engine.toJSValue(123)); }
            catch (...) { threwNonPtr = true; }
            check("conv fromJSValue<PP*> non-pointer throws", threwNonPtr);

            PP obj2; obj2.v = 5;
            JZJSValuePtr jsOwned = engine.toValuePtr(&obj2, true);
            auto& cp = engine.fromJSValue<jsbind::ClassPointer<PP>>(jsOwned.get());
            cp.clear();
            bool threwCleared = false;
            try { (void)engine.fromJSValue<PP*>(jsOwned.get()); }
            catch (...) { threwCleared = true; }
            check("conv fromJSValue<PP*> cleared throws", threwCleared);
        }

        engine.deinit();
        engine.init();   // 恢复干净会话
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
        
        jzDebug() << "[loopback] 持续 " << secs << "s:"
                  << " loopback 圈数=" << cpp_calls
                  << " (" << (long long)(cpp_calls / secs) << " 圈/s, 每圈=C++ 异步被回调一次)"                  
                  << " (微任务在单轮 pump 内自再生, 空闲不唤醒)";
        check("async loopback keeps driving", cpp_calls >= 100);
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
