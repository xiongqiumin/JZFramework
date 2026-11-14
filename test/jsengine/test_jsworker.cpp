// JZJSWorkerModule 端到端测试
//
// 覆盖(v1 语义):
//   1. __jz.worker 原子 API 就位: selfId / parentId / isMainThread / ids / childIds
//   2. worker 里是全新空环境: 不继承父线程的全局变量, isMainThread=false, parentId=0
//   3. workerData: 创建时序列化进去(对象/数组/字符串/数字/bool/null/嵌套)
//   4. 4 个 worker 分区间求和 -> 主线程汇总(证明各自独立引擎 + 消息通信)
//   5. 全局按 id 通信: worker A -> worker B(不经过主线程) -> 主线程
//   6. worker 里再 new Worker(嵌套) + 级联终止
//   7. terminate() -> 'exit' 事件; 脚本里 __jz.worker.exit() -> 'exit' 事件
//   8. 消息载荷支持共享引用(REFERENCE 模式)
//
// 说明: worker 入口脚本写在临时目录里, 主引擎 workDir 指向该目录, worker 引擎继承同一 workDir。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "JZApplication.h"
#include "JZDebug.h"
#include "JZDir.h"
#include "JZEventLoop.h"
#include "JZJSCoreModule.h"
#include "JZJSEngine.h"
#include "JZJSWorkerModule.h"
#include "JZTimer.h"

using namespace std;

static int g_fail = 0;
static FILE* g_log = NULL;

// 测试日志: 直接写文件并 flush —— 即使进程最后被强杀也能拿到每一步结果
static void logLine(const string& s)
{
    if (!g_log)
        return;
    fprintf(g_log, "%s\n", s.c_str());
    fflush(g_log);
}

static void check(const string& name, bool ok)
{
    logLine(string(ok ? "[PASS] " : "[FAIL] ") + name);
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

#define STEP(x) do { logLine(string("[step] ") + x); } while (0)

static bool writeTextFile(const string& path, const string& text)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

int test_jsworker(int argc, char *argv[])
{
    STEP("enter main");
    setbuf(stdout, NULL);
    {
        string logPath = JZDir::currentPath() + "/_jsworker_test_log.txt";
        g_log = fopen(logPath.c_str(), "wb");
    }

    // 看门狗: 线程/消息类测试一旦实现有问题很容易永久挂起, 这里兜底强退,
    // 免得 CI 上一直吊着(退出码 99 表示"卡死"而不是断言失败)。
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        logLine("[watchdog] TIMEOUT -> force exit");
        fflush(NULL);
        _Exit(99);   // <cstdlib>: 立即终止, 不做 flush/静态析构(看门狗语义)
    }).detach();

    JZEventLoop loop;

    JZJSEngine engine;
    engine.init();
    connect(&engine.sigLog, [](const string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string& s) { jzDebug() << "[js:error] " << s; });

    // 运行根 = 当前工作目录下的临时目录(worker 入口脚本都放这里)
    string root = JZDir::currentPath() + "/_jsworker_test";
    {
        JZDir d;
        d.mkpath(root);
    }
    engine.setWorkDir(root);
    // 运行环境的模块查找目录(与生产一致): net 模块注册时要按裸名装载 Web 全局包
    // (events/url/ws/http), worker 引擎继承同一份 ⇒ worker 里也能零 import 用 fetch/URL/WebSocket。
    engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });
    engine.start();

    // 内置模块(core) —— 与 worker 线程内的装配保持一致
    JZJSCoreModule coreModule;
    coreModule.initEngine(&engine);

    // 主线程侧的 worker 模块(id=0): 主线程也要登记, 否则 worker 投不回来。
    // 【必须最后装】net 会无条件重建 __jz, 而本模块往 __jz 里塞 worker 子命名空间。
    JZJSWorkerModule workerModule;
    workerModule.initEngine(&engine, 0, -1, JZByteArray());

    // ---- 测试用 JS 入口脚本 ----
    // 注意: 这里写出的 JS 必须全是 ASCII —— 源码里的中文字面量在 MSVC(未加 /utf-8)下
    // 会被按本地编码解成非法 UTF-8 字节, 落到磁盘后 quickjs 编译模块直接失败。
    bool wrote = true;
    wrote &= writeTextFile(root + "/basic.js",
        "// echo: reply to sender with env info\n"
        "__jz.worker.onMessage(function (from, value) {\n"
        "  __jz.worker.postTo(from, {\n"
        "    kind: 'echo', got: value,\n"
        "    myId: __jz.worker.selfId(), isMain: __jz.worker.isMainThread(),\n"
        "    parent: __jz.worker.parentId(),\n"
        "    leak: (typeof globalThis.leak),\n"
        "    hasLeak: ('leak' in globalThis)\n"
        "  });\n"
        "});\n");

    wrote &= writeTextFile(root + "/data.js",
        "// send workerData back\n"
        "__jz.worker.postTo(0, { data: __jz.worker.workerData, isMain: __jz.worker.isMainThread() });\n");

    wrote &= writeTextFile(root + "/sum.js",
        "// range sum (cpu bound)\n"
        "__jz.worker.onMessage(function (from, task) {\n"
        "  let s = 0;\n"
        "  for (let i = task.from; i <= task.to; i++) s += i;\n"
        "  __jz.worker.postTo(from, { id: __jz.worker.selfId(), from: task.from, to: task.to, sum: s });\n"
        "});\n");

    wrote &= writeTextFile(root + "/leaf.js",
        "// leaf for nested worker: echo\n"
        "__jz.worker.onMessage(function (from, v) {\n"
        "  __jz.worker.postTo(from, { op: 'pong', echo: v.payload, leafId: __jz.worker.selfId() });\n"
        "});\n");

    wrote &= writeTextFile(root + "/nested.js",
        "// spawn a child worker\n"
        "const child = new Worker('leaf.js');\n"
        "__jz.worker.onMessage(function (from, v) {\n"
        "  if (v.op === 'childId') { __jz.worker.postTo(from, { op: 'childId', childId: child.id }); return; }\n"
        "  if (v.op === 'ping')    { __jz.worker.postTo(child.id, { op: 'ping', payload: v.payload }); return; }\n"
        "  if (v.op === 'pong')    { __jz.worker.postTo(0, { op: 'pong', echo: v.echo, childId: child.id }); return; }\n"
        "});\n");

    wrote &= writeTextFile(root + "/loop.js",
        "// resident worker (for terminate)\n"
        "let n = 0;\n"
        "setInterval(function () { n = n + 1; }, 50);\n"
        "__jz.worker.postTo(0, { ready: true, id: __jz.worker.selfId() });\n");

    wrote &= writeTextFile(root + "/quit.js",
        "// exit by itself\n"
        "__jz.worker.postTo(0, { bye: true });\n"
        "__jz.worker.exit();\n");

    check("write worker entry scripts", wrote);

    // ---- 轮询: 泵事件循环直到 JS 表达式为真 ----
    auto waitFor = [&](const string& expr, int timeoutMs) {
        bool done = false;
        JZTimer poll;
        connect(&poll.sigTimeout, [&]() {
            auto v = engine.eval(expr);
            if (!engine.isException(v.get()) && engine.fromJSValue<bool>(v.get()))
            {
                done = true;
                loop.quit();
            }
        });
        poll.start(20);

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&]() { loop.quit(); });
        wd.start(timeoutMs);

        loop.exec();
        poll.stop();
        wd.stop();
        return done;
    };

    auto evalStr = [&](const string& expr) -> string {
        auto v = engine.eval(expr);
        if (engine.isException(v.get()))
            return string("<exception>");
        return engine.fromJSValue<string>(v.get());
    };

    auto evalBool = [&](const string& expr) -> bool {
        auto v = engine.eval(expr);
        if (engine.isException(v.get()))
            return false;
        return engine.fromJSValue<bool>(v.get());
    };

    // 收件箱 / 退出表 / 错误表
    {
        auto v = engine.eval(
            "globalThis.__inbox = []; globalThis.__exits = [];\n"
            "__jz.worker.onMessage(function (from, value) { __inbox.push({ from: from, value: value }); });\n"
            "globalThis.__err = [];\n"
            "true");
        check("bootstrap inbox (main)", !engine.isException(v.get()));
    }

    // 1. 主线程侧原子 API
    {
        bool ok = evalBool("__jz.worker.isMainThread() === true")
               && evalBool("__jz.worker.selfId() === 0")
               && evalBool("__jz.worker.parentId() === -1")
               && evalBool("typeof __jz.worker.postTo === 'function'")
               && evalBool("typeof __jz.worker.ids === 'function'")
               && evalBool("__jz.worker.ids().length === 1");
        check("1 main-thread atomic api", ok);
    }

    // 2. worker 里是全新空环境 + 不继承父线程全局变量
    {
        engine.eval("globalThis.leak = 'from-main'");
        engine.eval(
            "globalThis.w1 = new Worker('basic.js');\n"
            "w1.on('exit', function (code) { __exits.push({ id: w1.id, code: code }); });\n"
            "true");
        engine.eval("w1.postMessage({ hello: 'world', n: 42 })");

        bool got = waitFor("__inbox.length >= 1", 2000);
        check("2 worker echo received", got);

        string myId = evalStr("String(__inbox[0].value.myId)");
        bool ok = got
               && evalStr("String(__inbox[0].value.kind)") == "echo"
               && evalStr("String(__inbox[0].value.got.hello)") == "world"
               && evalStr("String(__inbox[0].value.got.n)") == "42"
               && evalStr("String(__inbox[0].value.isMain)") == "false"
               && evalStr("String(__inbox[0].value.parent)") == "0"
               && evalStr("String(__inbox[0].from)") == myId          // 消息带发送方 id
               && evalStr("String(__inbox[0].value.hasLeak)") == "false";  // 不继承全局变量
        jzDebug() << "[2] from=" << evalStr("String(__inbox[0].from)") << " myId=" << myId
                  << " leak=" << evalStr("String(__inbox[0].value.leak)")
                  << " hasLeak=" << evalStr("String(__inbox[0].value.hasLeak)");
        check("2 worker env independent (no global inherit)", ok);
    }

    // 3. workerData(各种类型) + 主线程 ids 里能查到
    {
        engine.eval(
            "globalThis.w2 = new Worker('data.js', { workerData: {\n"
            "  n: 7, s: 'hi', b: true, z: null, arr: [1, 2, 3],\n"
            "  obj: { deep: { k: 'v' } }\n"
            "} });\n"
            "w2.on('exit', function (code) { __exits.push({ id: w2.id, code: code }); });\n"
            "true");

        bool got = waitFor("__inbox.some(function (m) { return m.value.data && m.value.data.n === 7; })", 2000);
        check("3 workerData delivered", got);
        if (got)
        {
            bool ok = evalStr("String(__inbox.filter(function(m){return m.value.data&&m.value.data.n===7;})[0].value.data.s)") == "hi"
                   && evalStr("String(__inbox.filter(function(m){return m.value.data&&m.value.data.n===7;})[0].value.data.b)") == "true"
                   && evalStr("String(__inbox.filter(function(m){return m.value.data&&m.value.data.n===7;})[0].value.data.z)") == "null"
                   && evalStr("String(__inbox.filter(function(m){return m.value.data&&m.value.data.n===7;})[0].value.data.arr.join(','))") == "1,2,3"
                   && evalStr("String(__inbox.filter(function(m){return m.value.data&&m.value.data.n===7;})[0].value.data.obj.deep.k)") == "v";
            check("3 workerData types (num/str/bool/null/array/nested)", ok);
        }
        else
        {
            check("3 workerData types (num/str/bool/null/array/nested)", false);
        }
    }

    // 4. 4 个 worker 分区间求和 -> 汇总
    {
        engine.eval(
            "globalThis.__sumWorkers = [];\n"
            "const __N = 1000000;\n"
            "for (let i = 0; i < 4; i++) {\n"
            "  const w = new Worker('sum.js');\n"
            "  __sumWorkers.push(w);\n"
            "  w.on('exit', function (code) { __exits.push({ id: w.id, code: code }); });\n"
            "  w.postMessage({ from: i * __N + 1, to: (i + 1) * __N });\n"
            "}\n"
            "true");
        bool got = waitFor("__inbox.filter(function(m){return m.value&&m.value.sum!==undefined;}).length >= 4", 4000);
        check("4 four workers answered", got);

        engine.eval(
            "globalThis.__sumTotal = 0;\n"
            "for (const m of __inbox) { if (m.value && m.value.sum !== undefined) __sumTotal += m.value.sum; }\n"
            "globalThis.__sumIds = new Set();\n"
            "for (const m of __inbox) { if (m.value && m.value.sum !== undefined) __sumIds.add(m.value.id); }\n"
            "true");
        // 1..4,000,000 的和 = 4000000*4000001/2
        string total = evalStr("String(__sumTotal)");
        string expected = "8000002000000";
        string ids = evalStr("String(__sumIds.size)");
        jzDebug() << "[4] total=" << total << " expect=" << expected << " distinctWorkerIds=" << ids;
        check("4 partial sums correct", total == expected);
        check("4 four distinct worker engines", ids == "4");
    }

    // 5. 多个 worker 并存: id 互不相同 + 注册表可见 + 全局按 id 投递
    {
        engine.eval(
            "globalThis.__a = new Worker('basic.js');\n"
            "globalThis.__b = new Worker('basic.js');\n"
            "__a.on('exit', function (c) { __exits.push({ id: __a.id, code: c }); });\n"
            "__b.on('exit', function (c) { __exits.push({ id: __b.id, code: c }); });\n"
            "true");

        string aId = evalStr("String(__a.id)");
        string bId = evalStr("String(__b.id)");
        bool listed = evalBool("__jz.worker.ids().indexOf(__a.id) >= 0 && __jz.worker.ids().indexOf(__b.id) >= 0");
        jzDebug() << "[5] a=" << aId << " b=" << bId;
        check("5 two workers: distinct ids + listed in registry", aId != bId && listed);

        // 全局按 id 投递(不走 Worker 句柄): 收方回声给发送方, from 应为 b 的 id
        engine.eval("__jz.worker.postTo(__b.id, { direct: 'to-b' })");
        bool got = waitFor("__inbox.filter(function(m){return m.value&&m.value.got&&m.value.got.direct==='to-b';}).length >= 1", 2000);
        check("5 global postTo by id", got);
        if (got)
        {
            string from = evalStr("String(__inbox.filter(function(m){return m.value&&m.value.got&&m.value.got.direct==='to-b';})[0].from)");
            jzDebug() << "[5] direct echo from=" << from << " (bId=" << bId << ")";
            check("5 reply carries real sender id", from == bId);
        }
    }

    // 6. 嵌套 worker + 级联终止
    {
        engine.eval(
            "globalThis.__n = new Worker('nested.js');\n"
            "__n.on('exit', function (c) { __exits.push({ id: __n.id, code: c }); });\n"
            "true");
        engine.eval("__n.postMessage({ op: 'childId' })");
        bool gotId = waitFor("__inbox.filter(function(m){return m.value&&m.value.op==='childId';}).length >= 1", 2000);

        string childId = evalStr("String(__inbox.filter(function(m){return m.value&&m.value.op==='childId';})[0].value.childId)");
        string nestedId = evalStr("String(__n.id)");
        jzDebug() << "[6] nested=" << nestedId << " child=" << childId;

        bool childIdOk = gotId && childId != "undefined" && childId != "<exception>" && childId != nestedId;
        bool ok = childIdOk && evalBool("__jz.worker.ids().indexOf(" + childId + ") >= 0");
        check("6 worker-in-worker spawned (nested id in registry)", ok);

        // 嵌套 worker 的 ping/pong: 主线程 -> nested -> leaf -> nested -> 主线程
        engine.eval("__n.postMessage({ op: 'ping', payload: 'deep' })");
        bool gotPong = waitFor("__inbox.filter(function(m){return m.value&&m.value.op==='pong'&&m.value.echo==='deep';}).length >= 1", 2000);
        check("6 nested round trip (main -> nested -> leaf -> main)", gotPong);

        // terminate 嵌套 worker -> 其子 worker 应被级联收掉
        engine.eval("__n.terminate()");
        bool exited = waitFor("__exits.filter(function(e){return String(e.id) === '" + nestedId + "';}).length >= 1", 2000);
        check("6 terminate nested worker -> exit event", exited);

        if (childIdOk)
        {
            bool childGone = waitFor("__jz.worker.ids().indexOf(" + childId + ") < 0", 2000);
            check("6 cascade: leaf worker reaped with parent", childGone);
        }
        else
        {
            check("6 cascade: leaf worker reaped with parent", false);
        }
    }

    // 7. terminate 常驻 worker / 脚本自退出
    {
        engine.eval(
            "globalThis.__loopW = new Worker('loop.js');\n"
            "__loopW.on('exit', function (c) { __exits.push({ id: __loopW.id, code: c }); });\n"
            "true");
        bool ready = waitFor("__inbox.filter(function(m){return m.value&&m.value.ready;}).length >= 1", 2000);
        check("7 resident worker started", ready);

        string loopId = evalStr("String(__loopW.id)");
        engine.eval("__loopW.terminate()");
        bool exited = waitFor("__exits.filter(function(e){return String(e.id) === '" + loopId + "';}).length >= 1", 2000);
        check("7 terminate() stops resident worker", exited);
        check("7 handle marked exited", evalBool("__loopW.exited === true"));

        engine.eval(
            "globalThis.__quitW = new Worker('quit.js');\n"
            "__quitW.on('exit', function (c) { __exits.push({ id: __quitW.id, code: c }); });\n"
            "true");
        string quitId = evalStr("String(__quitW.id)");
        bool bye = waitFor("__inbox.filter(function(m){return m.value&&m.value.bye;}).length >= 1", 2000);
        bool quitExited = waitFor("__exits.filter(function(e){return String(e.id) === '" + quitId + "';}).length >= 1", 2000);
        check("7 __jz.worker.exit() ends worker", bye && quitExited);
    }

    // 8. 消息支持共享引用/循环引用(REFERENCE 模式)
    {
        engine.eval("w1.postMessage({ a: { k: 1 }, b: null })");   // 先占位
        bool ok = false;
        {
            auto v = engine.eval(
                "globalThis.__shared = { tag: 'shared' };\n"
                "globalThis.__cyc = { name: 'cycle' }; __cyc.self = __cyc;\n"
                "w1.postMessage({ x: __shared, y: __shared, z: __cyc });\n"
                "true");
            ok = !engine.isException(v.get());
        }
        check("8 send object with shared/cyclic refs", ok);

        bool got = waitFor("__inbox.filter(function(m){return m.value&&m.value.got&&m.value.got.z&&m.value.got.z.name==='cycle';}).length >= 1", 2000);
        check("8 cyclic payload delivered", got);
        if (got)
        {
            string same = evalStr(
                "String((function(){const m=__inbox.filter(function(m){return m.value&&m.value.got&&m.value.got.z&&m.value.got.z.name==='cycle';}).pop();"
                "return (m.value.got.x === m.value.got.y) && (m.value.got.z.self === m.value.got.z);})())");
            check("8 shared ref identity + cycle preserved", same == "true");
        }
    }

    // 9. 收尾: 全部 worker 退出后进程表里只剩主线程
    {
        engine.eval(
            "const __kill = [];\n"
            "if (typeof w1 !== 'undefined') __kill.push(w1);\n"
            "if (typeof w2 !== 'undefined') __kill.push(w2);\n"
            "if (typeof __a !== 'undefined') __kill.push(__a);\n"
            "if (typeof __b !== 'undefined') __kill.push(__b);\n"
            "if (typeof __sumWorkers !== 'undefined') { for (const w of __sumWorkers) __kill.push(w); }\n"
            "for (const w of __kill) { try { w.terminate(); } catch (e) {} }\n"
            "true");
        bool allGone = waitFor("__jz.worker.ids().length === 1", 3000);
        check("9 all workers reaped (only main left)", allGone);
    }

    // 诊断: 打印 JS 侧最终状态(失败时用于定位)
    logLine("[diag] inbox=" + evalStr("String(__inbox.length)")
          + " ids=" + evalStr("JSON.stringify(__jz.worker.ids())")
          + " exits=" + evalStr("JSON.stringify(__exits)"));
    logLine("[diag] uid=" + evalStr("String(__jz.worker.selfId())")
          + " isMain=" + evalStr("String(__jz.worker.isMainThread())")
          + " root=" + root);

    STEP("done");
    jzDebug() << "[result] fail =" << g_fail;
    return g_fail;
}
