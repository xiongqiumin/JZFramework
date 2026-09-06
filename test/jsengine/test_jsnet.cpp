// JZJSNetModule 端到端测试: 全部走本机 loopback, 不依赖外网
#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
#include "JZJSCoreModule.h"
#include "JZTimer.h"

using namespace std;

static int g_fail = 0;

static void check(const string& name, bool ok)
{
    fprintf(stderr, "[check] %s\n", name.c_str());
    fflush(stderr);
    jzDebug() << (ok ? "[PASS]" : "[FAIL]") << name;
    if (!ok)
        g_fail++;
}

#define STEP(x) do { fprintf(stderr, "[step] %s\n", x); fflush(stderr); } while (0)

int main()
{
    STEP("enter main");
    JZApplication a;
    STEP("app created");
    JZEventLoop loop;
    jzco_init();

    JZJSEngine engine;
    engine.init();
    STEP("engine init");
    connect(&engine.sigLog, [](const string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string& s) { jzDebug() << "[js:error] " << s; });
    engine.start();
    STEP("engine start");

    JZJSNetModule netModule;
    netModule.initEngine(&engine);
    STEP("net module init");

    JZJSCoreModule coreModule;
    coreModule.initEngine(&engine);
    STEP("core module init");

    // 轮询等待某个 JS 表达式为真(同时泵事件循环, 让网络回调落地)
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
        poll.start(50);

        JZTimer wd;
        wd.setSingleShot(true);
        connect(&wd.sigTimeout, [&]() { loop.quit(); });
        wd.start(timeoutMs);

        loop.exec();
        poll.stop();
        wd.stop();
        return done;
    };

    // 1. 模块注入: 全局 + require 都可用
    {
        auto v = engine.eval(
            "typeof net === 'object' && typeof dgram === 'object' && typeof http === 'object'"
            " && typeof ws === 'object' && typeof mqtt === 'object' && typeof os === 'object'"
            " && require('net') === net && typeof fetch === 'function'"
            " && typeof WebSocket === 'function' && typeof URL === 'function'");
        check("modules injected (global + require)", !engine.isException(v.get()) &&
                                                        engine.fromJSValue<bool>(v.get()));
    }

    // 2. URL / URLSearchParams
    {
        auto v = engine.eval(
            "(function(){ var u = new URL('http://host:8080/a/b?x=1&y=2');"
            " return u.protocol + '|' + u.hostname + '|' + u.port + '|' + u.pathname"
            " + '|' + u.search + '|' + u.searchParams.get('x'); })()");
        string got = engine.isException(v.get()) ? string("<exception>") : engine.fromJSValue<string>(v.get());
        jzDebug() << "URL got:" << got << " expect: http:|host|8080|/a/b|?x=1&y=2|1";
        check("URL parse", got == "http:|host|8080|/a/b|?x=1&y=2|1");
    }

    // 3. os.networkInterfaces
    {
        auto v = engine.eval("Object.keys(os.networkInterfaces()).length > 0");
        check("os.networkInterfaces", !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get()));
    }

    // 4. EventEmitter 语义(once / off / emit)
    {
        auto v2 = engine.eval(
            "(function(){ var log = ''; var e = new EventEmitter();"
            " function a(){ log += 'a'; }"
            " e.on('x', a); e.once('x', function(){ log += 'o'; });"
            " e.emit('x'); e.emit('x'); e.off('x', a); e.emit('x');"
            " return log; })()");
        // on(a) + once(o): emit -> "ao"; emit -> "a"(once 已消耗); off(a); emit -> 无
        check("EventEmitter on/once/off", !engine.isException(v2.get()) &&
                                              engine.fromJSValue<string>(v2.get()) == "aoa");
    }

    // 5. TCP: server echo + client 收发
    {
        engine.eval(
            "globalThis.__echo = '';"
            "(function(){ var srv = net.createServer(function(sock){"
            "   sock.on('data', function(d){ sock.write(d); });"
            " });"
            " srv.listen(19876, '127.0.0.1', function(){"
            "   var c = net.connect(19876, '127.0.0.1', function(){ c.write('hello'); });"
            "   c.on('data', function(d){ globalThis.__echo = __jz.bytes.toString(d); });"
            " }); })()");
        bool ok = waitFor("globalThis.__echo === 'hello'", 5000);
        check("net tcp echo", ok);
    }

    // 6. TCP: 二进制与编码(hex)
    {
        engine.eval(
            "globalThis.__bin = '';"
            "(function(){ var c = net.connect(19876, '127.0.0.1', function(){"
            "   c.write(Uint8Array.fromHex('0102ff'));"
            " });"
            " c.on('data', function(d){ globalThis.__bin = d.toHex(); }); })()");
        bool ok = waitFor("globalThis.__bin === '0102ff'", 5000);
        check("net tcp binary + hex", ok);
    }

    // 7. UDP: 自发自收
    {
        engine.eval(
            "globalThis.__udp = '';"
            "(function(){ var s = dgram.createSocket('udp4');"
            " s.on('message', function(msg, rinfo){"
            "   globalThis.__udp = __jz.bytes.toString(msg) + ':' + rinfo.port; });"
            " s.bind(19877, function(){ s.send('ping', 19877, '127.0.0.1'); }); })()");
        bool ok = waitFor("globalThis.__udp === 'ping:19877'", 5000);
        check("dgram send/recv", ok);
    }

    // 8. WebSocket: 服务端 + 客户端 端到端
    {
        engine.eval(
            "globalThis.__ws = '';"
            "(function(){ var wss = new ws.WebSocketServer({ port: 19878 });"
            " wss.on('listening', function(){ console.error('WSS_LISTENING'); });"
            " wss.on('connection', function(sock){"
            "   console.error('WSS_CONN');"
            "   sock.on('message', function(m){ console.error('WSS_MSG', typeof m, m); sock.send('echo:' + m); });"
            " });"
            " var c = new WebSocket('ws://127.0.0.1:19878');"
            " c.onopen = function(){ console.error('C_OPEN'); c.send('hi'); };"
            " c.onmessage = function(ev){ console.error('C_MSG', ev.data); globalThis.__ws = ev.data; };"
            " c.onerror = function(e){ console.error('C_ERR', String(e)); }; })()");
        bool ok = waitFor("globalThis.__ws === 'echo:hi'", 6000);
        auto v = engine.eval("globalThis.__ws");
        jzDebug() << "ws result:" << (engine.isException(v.get()) ? string("<exception>")
                                                                   : engine.fromJSValue<string>(v.get()));
        check("websocket server + client", ok);
    }

    // 9. http/fetch: 连接失败走 reject(同时验证 pendingWork 会释放)
    //    先用 net.Server 占一个固定端口再关闭, 得到确定无监听的端口
    {
        engine.eval(
            "globalThis.__httpErr = '';"
            "(function(){"
            "  var net = require('net');"
            "  var s = net.createServer();"
            "  s.listen(19879, '127.0.0.1', function(){"
            "    var p = s.address().port;"
            "    s.close();"
            "    s.once('close', function(){"
            "      fetch('http://127.0.0.1:' + p + '/').then(function(){"
            "        globalThis.__httpErr = 'NO_ERROR'; console.error('FETCH_OK');"
            "      }, function(e){ globalThis.__httpErr = 'ERR:' + (e && e.message ? e.message : String(e)); console.error('FETCH_ERR', String(e)); });"
            "    });"
            "  });"
            "})()");
        bool ok = waitFor("globalThis.__httpErr !== ''", 8000);
        auto v = engine.eval("globalThis.__httpErr");
        jzDebug() << "fetch result:" << (engine.isException(v.get()) ? string("<exception>")
                                                                     : engine.fromJSValue<string>(v.get()));
        check("fetch rejects on connect error", ok && engine.pendingWork() == 0);
    }

    // 10. mqtt: 连不上时不能崩, 且 client 对象可用
    {
        auto v = engine.eval(
            "(function(){ var c = mqtt.connect('mqtt://127.0.0.1:1', { clientId: 'js' });"
            " return typeof c.publish === 'function' && c.connected === false; })()");
        check("mqtt client created", !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get()));
    }

    // 11. core module: fs/path/os/process 同步 API(对齐 Node)
    {
        auto v = engine.eval(
            "(function(){"
            "  var fs = require('fs');"
            "  var path = require('path');"
            "  var os = require('os');"
            "  var bad = [];"
            "  var f = 'jzcore_test_tmp.txt';"
            "  try { fs.unlinkSync(f); } catch(e){}"
            "  fs.writeFileSync(f, 'hello', 'utf8');"
            "  if (fs.readFileSync(f, 'utf8') !== 'hello') bad.push('ok1');"
            "  var st = fs.statSync(f);"
            "  if (!(st.isFile() && st.size === 5)) bad.push('ok2');"
            "  fs.appendFileSync(f, '-world');"
            "  if (fs.readFileSync(f, 'utf8') !== 'hello-world') bad.push('ok3');"
            "  var u8 = fs.readFileSync(f);"
            "  if (!(u8 instanceof Uint8Array) || u8.length !== 11) bad.push('ok4');"
            "  var dir = 'jzcore_test_dir';"
            "  try { fs.rmSync(dir, {recursive:true}); } catch(e){}"
            "  fs.mkdirSync(dir, {recursive:true});"
            "  fs.writeFileSync(dir + '/a.txt', 'x');"
            "  var names = fs.readdirSync(dir);"
            "  if (names.indexOf('a.txt') < 0) bad.push('ok5');"
            "  var dt = fs.readdirSync(dir, {withFileTypes:true});"
            "  if (!(dt.length === 1 && dt[0].isFile() && dt[0].name === 'a.txt')) bad.push('ok6');"
            "  fs.rmSync(dir, {recursive:true});"
            "  if (fs.existsSync(dir)) bad.push('ok7');"
            "  if (path.basename('/a/b/c.txt') !== 'c.txt') bad.push('ok8');"
            "  if (path.extname('/a/b/c.txt') !== '.txt') bad.push('ok9');"
            "  if (path.dirname('/a/b/c.txt') !== '/a/b') bad.push('ok10');"
            "  if (path.join('/a','b','c') !== '/a/b/c') bad.push('ok11');"
            "  if (!(path.isAbsolute('/a/b') === true && path.isAbsolute('a/b') === false)) bad.push('ok12');"
            "  if (typeof os.platform() !== 'string' || os.platform().length === 0) bad.push('ok13');"
            "  if (typeof os.tmpdir() !== 'string') bad.push('ok14');"
            "  if (typeof process.cwd() !== 'string' || process.cwd().length === 0) bad.push('ok15');"
            "  fs.unlinkSync(f);"
            "  return bad.join(',');"
            "})()");
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<string>(v.get()).empty();
        if (!ok && !engine.isException(v.get()))
            jzDebug() << "[core-err] failed checks: " << engine.fromJSValue<string>(v.get());
        if (engine.isException(v.get()))
            jzDebug() << "[core-err] exception";
        check("core fs/path/os/process sync API", ok);
    }

    STEP("deinit module");
    coreModule.deinitEngine();
    netModule.deinitEngine();
    STEP("engine stop");
    engine.stop();
    STEP("engine deinit");
    engine.deinit();
    STEP("done");

    jzDebug() << (g_fail == 0 ? "ALL PASS" : ("FAILED: " + to_string(g_fail)));
    return g_fail == 0 ? 0 : 1;
}
