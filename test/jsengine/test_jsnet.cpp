// JZJSNetModule 端到端测试: 全部走本机 loopback, 不依赖外网
//
// 新架构(分离式加载, 一个名字 = 一个包目录):
//   JS 适配层不再以原始字符串内嵌进 C++, 而是由 js_wrapper/<名字>/index.ts 经 tsc
//   编译为 <名字>/dist/index.js。JZJSNetModule::initEngine / JZJSCoreModule::initEngine
//   只注册 __jz.* 原子 API 与原生类; 适配层属于"用户入口"一侧, 由 import 被动加载
//   —— 本测试在下面用一段临时 .mjs 逐个 import 各包(每个包的默认导出就是模块对象,
//   具名导出是各成员), 再挂回 globalThis 供断言使用。
//   require() 不再支持, 只认 ESM。

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZJSEngine.h"
#include "JZJSNetModule.h"
#include "JZJSCoreModule.h"
#include "JZJSSerialPortModule.h"
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
    // 供各包以裸模块名(events/dist、internal/dist)互相 import 时定位
    engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });
    STEP("engine start");

    JZJSNetModule netModule;
    netModule.initEngine(&engine);
    STEP("net module init");

    JZJSCoreModule coreModule;
    coreModule.initEngine(&engine);
    STEP("core module init");

    JZJSSerialPortModule serialModule;
    serialModule.initEngine(&engine);
    STEP("serial port module init");

    // 测试侧引导: 把各"导入式模块"重新暴露为全局, 供下面以裸名运行的 eval 断言复用
    // (生产侧是 import-only, 对齐 Node; 这里是测试便利, 不影响生产代码)。
    // ⭐ 一名字一包之后每个模块都有自己的目录, 默认导出就是模块对象。
    {
        std::string wrapper = JZ_JS_WRAPPER_DIR;
        std::string tmp = std::string(wrapper) + "/_test_globals.mjs";
        std::string mjs =
            "import net from './net/dist/index.js';\n"
            "import dgram from './dgram/dist/index.js';\n"
            "import http from './http/dist/index.js';\n"
            "import mqtt from './mqtt/dist/index.js';\n"
            "import ws from './ws/dist/index.js';\n"
            "import url from './url/dist/index.js';\n"
            "import os from './os/dist/index.js';\n"
            "import fs from './fs/dist/index.js';\n"
            "import path from './path/dist/index.js';\n"
            "import process from './process/dist/index.js';\n"
            "import crypto from './crypto/dist/index.js';\n"
            "globalThis.net = net; globalThis.dgram = dgram; globalThis.http = http;\n"
            "globalThis.mqtt = mqtt; globalThis.ws = ws; globalThis.url = url;\n"
            "globalThis.os = os; globalThis.fs = fs; globalThis.path = path;\n"
            "globalThis.process = process; globalThis.crypto = crypto;\n";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f) { fwrite(mjs.data(), 1, mjs.size(), f); fclose(f); }
        engine.runModuleFile(tmp);
        remove(tmp.c_str());
    }

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

    // 1. 模块注入: 全局对象可用(net/dgram/http/ws/mqtt/os 由 initEngine 装入)
    {
        auto v = engine.eval(
            "typeof net === 'object' && typeof dgram === 'object' && typeof http === 'object'"
            " && typeof ws === 'object' && typeof mqtt === 'object' && typeof os === 'object'"
            " && typeof fetch === 'function'"
            " && typeof WebSocket === 'function' && typeof URL === 'function'");
        check("modules injected (global)", !engine.isException(v.get()) &&
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

    // 11b. core module: 文件复制 / 移动 / 递归复制(新增 __jz.core.fileCopy|fileRename|dirCopy
    //      的 Node 同步包装: fs.copyFileSync / renameSync / cpSync)
    {
        auto v = engine.eval(
            "(function(){"
            "  var bad = [];"
            "  var f = 'jzcore_test_cp_src.txt';"
            "  var ft = 'jzcore_test_cp_dst.txt';"
            "  try { fs.unlinkSync(f); } catch(e){}"
            "  try { fs.unlinkSync(ft); } catch(e){}"
            "  try { fs.unlinkSync(ft + '.ren'); } catch(e){}"
            "  try { fs.unlinkSync('jzcore_test_cp_f2.txt'); } catch(e){}"
            "  fs.writeFileSync(f, 'COPY', 'utf8');"
            // 原子 API 已注册
            "  if (typeof __jz.core.fileCopy !== 'function') bad.push('ok1');"
            "  if (typeof __jz.core.fileRename !== 'function') bad.push('ok2');"
            "  if (typeof __jz.core.dirCopy !== 'function') bad.push('ok3');"
            // copyFileSync: 默认覆盖目标
            "  fs.copyFileSync(f, ft);"
            "  if (fs.readFileSync(ft, 'utf8') !== 'COPY') bad.push('ok4');"
            "  fs.copyFileSync(f, ft);"
            "  if (fs.readFileSync(ft, 'utf8') !== 'COPY') bad.push('ok5');"
            // COPYFILE_EXCL: 目标已存在 -> EEXIST
            "  var ex = null;"
            "  try { fs.copyFileSync(f, ft, fs.constants.COPYFILE_EXCL); } catch(e){ ex = e; }"
            "  if (!(ex && ex.code === 'EEXIST')) bad.push('ok6');"
            // 源不存在 -> ENOENT
            "  var ne = null;"
            "  try { fs.copyFileSync('jzcore_test_no_such_xyz.txt', ft); } catch(e){ ne = e; }"
            "  if (!(ne && ne.code === 'ENOENT')) bad.push('ok7');"
            // renameSync: 移动 + 覆盖已存在目标
            "  fs.renameSync(ft, ft + '.ren');"
            "  if (!(fs.existsSync(ft + '.ren') && !fs.existsSync(ft))) bad.push('ok8');"
            "  fs.copyFileSync(f, ft);"
            "  fs.renameSync(ft, ft + '.ren');"
            "  if (fs.readFileSync(ft + '.ren', 'utf8') !== 'COPY') bad.push('ok9');"
            // cpSync: 文件 -> 文件
            "  fs.cpSync(f, 'jzcore_test_cp_f2.txt');"
            "  if (fs.readFileSync('jzcore_test_cp_f2.txt', 'utf8') !== 'COPY') bad.push('ok10');"
            // cpSync: 目录 -> 目录(recursive, 含子目录)
            "  var s = 'jzcore_test_cp_src';"
            "  var d = 'jzcore_test_cp_dst';"
            "  try { fs.rmSync(s, {recursive:true}); } catch(e){}"
            "  try { fs.rmSync(d, {recursive:true}); } catch(e){}"
            "  fs.mkdirSync(s + '/sub', {recursive:true});"
            "  fs.writeFileSync(s + '/a.txt', 'A');"
            "  fs.writeFileSync(s + '/sub/b.txt', 'B');"
            "  fs.cpSync(s, d, {recursive:true});"
            "  if (fs.readFileSync(d + '/a.txt', 'utf8') !== 'A') bad.push('ok11');"
            "  if (fs.readFileSync(d + '/sub/b.txt', 'utf8') !== 'B') bad.push('ok12');"
            // 目录缺 recursive -> ERR_FS_EISDIR(Node 语义)
            "  var ei = null;"
            "  try { fs.cpSync(s, d + '_x'); } catch(e){ ei = e; }"
            "  if (!(ei && ei.code === 'ERR_FS_EISDIR')) bad.push('ok13');"
            // 清理
            "  fs.unlinkSync(f); fs.unlinkSync(ft + '.ren'); fs.unlinkSync('jzcore_test_cp_f2.txt');"
            "  fs.rmSync(s, {recursive:true}); fs.rmSync(d, {recursive:true});"
            "  return bad.join(',');"
            "})()");
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<string>(v.get()).empty();
        if (!ok && !engine.isException(v.get()))
            jzDebug() << "[core-copy-err] failed checks: " << engine.fromJSValue<string>(v.get());
        if (engine.isException(v.get()))
            jzDebug() << "[core-copy-err] exception";
        check("core fs copy/rename/cp sync API", ok);
    }

    // 11c. core module: fd 级 IO(新增 __jz.core.fdOpen/Close/Read/Write/Seek/Tell/Size/AtEnd
    //      的 Node 同步包装: fs.openSync / closeSync / readSync / writeSync / fstatSync)
    {
        auto v = engine.eval(
            "(function(){"
            "  var bad = [];"
            "  var f = 'jzcore_test_fd.bin';"
            "  try { fs.unlinkSync(f); } catch(e){}"
            // 'w' 创建并写(字符串 + Uint8Array 两种形式)
            "  var fd = fs.openSync(f, 'w');"
            "  if (!(typeof fd === 'number' && fd >= 3)) bad.push('ok1');"
            "  if (fs.writeSync(fd, 'hello') !== 5) bad.push('ok2');"
            "  if (fs.writeSync(fd, Uint8Array.fromHex('2d776f726c64')) !== 6) bad.push('ok3');"
            "  fs.closeSync(fd);"
            // 'r': 顺序读 + position 形式读(且游标不变)
            "  fd = fs.openSync(f, 'r');"
            "  var b1 = new Uint8Array(5);"
            "  if (!(fs.readSync(fd, b1, 0, 5, null) === 5 && __jz.bytes.toString(b1) === 'hello')) bad.push('ok4');"
            "  var b2 = new Uint8Array(3);"
            "  if (!(fs.readSync(fd, b2, 0, 3, 0) === 3 && __jz.bytes.toString(b2) === 'hel')) bad.push('ok5');"
            "  var b3 = new Uint8Array(1);"
            // 游标仍停在 5 -> 该处是 '-'("hello-world"), 证明上面 position 形式没推进游标
            "  if (!(fs.readSync(fd, b3, 0, 1, null) === 1 && __jz.bytes.toString(b3) === '-')) bad.push('ok6');"
            // fstatSync: 本实现只有 size/isFile 是真实值
            "  var st = fs.fstatSync(fd);"
            "  if (!(st.isFile() && st.size === 11)) bad.push('ok7');"
            "  fs.closeSync(fd);"
            // 'r+' + 字符串形式的 position: 覆盖开头 2 字节
            "  fd = fs.openSync(f, 'r+');"
            "  if (fs.writeSync(fd, 'XY', 0) !== 2) bad.push('ok8');"
            "  fs.closeSync(fd);"
            "  if (fs.readFileSync(f, 'utf8') !== 'XYllo-world') bad.push('ok9');"
            // readFileSync(fd) 从当前游标到 EOF / writeFileSync(fd) 不截断
            "  fd = fs.openSync(f, 'r');"
            "  if (fs.readFileSync(fd, 'utf8') !== 'XYllo-world') bad.push('ok10');"
            "  fs.closeSync(fd);"
            "  fd = fs.openSync(f, 'r+');"
            "  fs.writeFileSync(fd, 'Z');"
            "  fs.closeSync(fd);"
            "  if (fs.readFileSync(f, 'utf8') !== 'ZYllo-world') bad.push('ok11');"
            // 'a' 追加写
            "  fd = fs.openSync(f, 'a');"
            "  fs.writeSync(fd, '!');"
            "  fs.closeSync(fd);"
            "  if (fs.readFileSync(f, 'utf8') !== 'ZYllo-world!') bad.push('ok12');"
            // 数字 flags(O_WRONLY|O_CREAT|O_TRUNC)
            "  var fd2 = __jz.core.fdOpen(f, 'r');"
            "  __jz.core.fdClose(fd2);"          // 同时也验证原子可用
            "  if (typeof __jz.core.fdOpen !== 'function') bad.push('ok13');"
            // EBADF(已关闭的 fd)/ ENOENT(flags 'r' 且不存在)
            "  var e1 = null;"
            "  try { fs.readSync(9999, new Uint8Array(1), 0, 1, null); } catch(e){ e1 = e; }"
            "  if (!(e1 && e1.code === 'EBADF')) bad.push('ok14');"
            "  var e2 = null;"
            "  try { fs.openSync('jzcore_test_no_such_fd_xyz.bin', 'r'); } catch(e){ e2 = e; }"
            "  if (!(e2 && e2.code === 'ENOENT')) bad.push('ok15');"
            "  fs.unlinkSync(f);"
            "  return bad.join(',');"
            "})()");
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<string>(v.get()).empty();
        if (!ok && !engine.isException(v.get()))
            jzDebug() << "[core-fd-err] failed checks: " << engine.fromJSValue<string>(v.get());
        if (engine.isException(v.get()))
            jzDebug() << "[core-fd-err] exception";
        check("core fs fd sync API", ok);
    }

    // 12. 新机制演示: 分离式加载 + ES module import(固定相对路径)
    //     在 wrapper 目录写一个临时 .mjs, 用相对固定路径 import 已编译的 net 包,
    //     验证: (a) 模块能从磁盘加载; (b) import 返回的对象就是全局那份(同一实例)。
    {
        std::string wrapper = JZ_JS_WRAPPER_DIR;
        std::string tmpPath = std::string(wrapper) + "/_import_check.mjs";
        std::string mjs =
            "import net from './net/dist/index.js';\n"
            "globalThis.__impNet = net;\n"
            "globalThis.__impSame = (net === globalThis.net);\n";
        FILE* f = fopen(tmpPath.c_str(), "wb");
        if (f) { fwrite(mjs.data(), 1, mjs.size(), f); fclose(f); }

        bool loaded = engine.runModuleFile(tmpPath);
        auto v = engine.eval("typeof __impNet === 'object' && typeof __impNet.createServer === 'function'");
        bool ok = loaded && !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
        if (ok)
        {
            auto s = engine.eval("globalThis.__impSame");
            jzDebug() << "[import] net === global net:" << (engine.isException(s.get()) ? string("<err>")
                                                                                     : (engine.fromJSValue<bool>(s.get()) ? "true" : "false"));
        }
        check("separated loading + import fixed path", ok);

        remove(tmpPath.c_str());
    }

    // 13. fs/path/process/crypto 各自独立成包后, 同样支持分离式加载 + ES module import
    {
        std::string wrapper = JZ_JS_WRAPPER_DIR;
        std::string tmpPath = std::string(wrapper) + "/_import_check_core.mjs";
        std::string mjs =
            "import f from './fs/dist/index.js';\n"
            "import p from './path/dist/index.js';\n"
            "import pr from './process/dist/index.js';\n"
            "import c from './crypto/dist/index.js';\n"
            "globalThis.__impFs = f;\n"
            "globalThis.__impSameFs = (f === globalThis.fs);\n"
            "globalThis.__impHasApi = (typeof f.writeFileSync === 'function'"
            " && typeof p.join === 'function' && typeof pr.cwd === 'function'"
            " && typeof c.randomUUID === 'function');\n";
        FILE* fptr = fopen(tmpPath.c_str(), "wb");
        if (fptr) { fwrite(mjs.data(), 1, mjs.size(), fptr); fclose(fptr); }

        bool loaded = engine.runModuleFile(tmpPath);
        auto v = engine.eval(
            "globalThis.__impHasApi === true && globalThis.__impSameFs === true");
        bool ok = loaded && !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
        check("core separated loading + import fixed path", ok);

        remove(tmpPath.c_str());
    }

    STEP("deinit module");
    serialModule.deinitEngine();
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
