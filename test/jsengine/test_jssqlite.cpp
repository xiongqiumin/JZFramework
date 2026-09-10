// JZJSqliteModule 端到端测试: node:sqlite 风格封装的 SQLite 基本功能
//
// 架构(与 net/core/serialport 一致):
//   JZJSqliteModule::initEngine 只注册 __jz.sqlite 原子 API 与原生类 JZJSSqliteDb;
//   js_wrapper/sqlite/index.ts 经 tsc 编译为 sqlite/dist/index.js, 把原子 API 包装成
//   Node `node:sqlite` 风格的 DatabaseSync / StatementSync, 由用户入口 import 被动加载。
//
// 覆盖: 模块注入 / 内存库建表 CRUD / 位置与具名参数 / 类型往返(INTEGER/REAL/TEXT/BLOB
// /NULL/boolean) / 事务 / 文件库持久化 / 语句缓存复用 / columns() / iterate() / 错误处理。

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZCo.h"
#include "JZDebug.h"
#include "JZEventLoop.h"
#include "JZJSEngine.h"
#include "JZJSqliteModule.h"
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
    JZEventLoop loop;
    jzco_init();

    JZJSEngine engine;
    engine.init();
    connect(&engine.sigLog, [](const string& s) { jzDebug() << "[js:log] " << s; });
    connect(&engine.sigError, [](const string& s) { jzDebug() << "[js:error] " << s; });
    engine.start();
    STEP("engine start");

    JZJSqliteModule sqliteModule;
    sqliteModule.initEngine(&engine);
    STEP("sqlite module init");

    const string tmpDb = "test_jssqlite_tmp.db";
    remove(tmpDb.c_str());

    // 测试侧引导: 加载 sqlite 适配层并把导出挂成全局(生产侧是 import-only, 这里只为断言方便)
    {
        std::string wrapper = JZ_JS_WRAPPER_DIR;
        std::string tmp = std::string(wrapper) + "/_test_sqlite_globals.mjs";
        std::string mjs =
            "import { DatabaseSync, StatementSync, version, sqlite } from './sqlite/dist/index.js';\n"
            "globalThis.DatabaseSync = DatabaseSync; globalThis.StatementSync = StatementSync;\n"
            "globalThis.sqliteVersion = version; globalThis.sqlite = sqlite;\n";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (f)
        {
            fwrite(mjs.data(), 1, mjs.size(), f);
            fclose(f);
        }
        engine.runModuleFile(tmp);
        remove(tmp.c_str());
    }

    // 把一个 JS 表达式求值为 bool 并断言
    auto runCheck = [&](const string& name, const string& expr) {
        auto v = engine.eval(expr);
        bool ok = !engine.isException(v.get()) && engine.fromJSValue<bool>(v.get());
        check(name, ok);
    };

    // 0. 原子 API 与适配层注入
    runCheck("atomic api __jz.sqlite.openDb",
             "typeof __jz === 'object' && typeof __jz.sqlite === 'object'"
             " && typeof __jz.sqlite.openDb === 'function'"
             " && typeof __jz.sqlite.version === 'function'");
    runCheck("adapter injected (DatabaseSync/StatementSync)",
             "typeof DatabaseSync === 'function' && typeof StatementSync === 'function'"
             " && typeof sqliteVersion === 'string' && sqliteVersion.length > 0");
    runCheck("sqlite version >= 3.53",
             "sqliteVersion.split('.').map(function(n){return parseInt(n,10);})[0] === 3");

    // 1. 内存库: 建表 / 插入 / 查询 / 类型往返
    runCheck("memory db crud + types", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    db.exec('CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, flag INTEGER, data BLOB)');
    var ins = db.prepare('INSERT INTO t(name, score, flag, data) VALUES (?, ?, ?, ?)');
    var r = ins.run('alice', 1.5, true, new Uint8Array([1, 2, 3]));
    var ok = (r.changes === 1 && r.lastInsertRowid === 1);
    var row = db.prepare('SELECT * FROM t WHERE id = ?').get(1);
    ok = ok && row.name === 'alice' && row.score === 1.5 && row.flag === 1;
    ok = ok && (row.data instanceof Uint8Array) && row.data.length === 3 && row.data[2] === 3;
    var all = db.prepare('SELECT id FROM t').all();
    ok = ok && all.length === 1 && all[0].id === 1;
    db.close();
    return ok;
})()
)JS");

    // 2. 具名参数(裸名 + 带前缀) / 位置与具名混用 / 表达式参数
    runCheck("named parameters", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    db.exec('CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT)');
    var ins = db.prepare('INSERT INTO t(id, name) VALUES (:id, :name)');
    ins.run({ id: 7, name: 'bob' });              // 裸名(对齐上游 allowBareNamedParameters)
    ins.run({ ':id': 8, ':name': 'carol' });      // 完整键名
    ins.run({ name: 'dave' }, 9);                 // 具名 + 位置混用
    var n = db.prepare('SELECT COUNT(*) AS c FROM t').get().c;
    var g = db.prepare('SELECT name FROM t WHERE id = :id').get({ id: 8 });
    var v = db.prepare('SELECT ? AS a, ? AS b').get(null, 42);
    db.close();
    return n === 3 && g.name === 'carol' && v.a === null && v.b === 42;
})()
)JS");

    // 3. 事务: isTransaction 状态 + 回滚
    runCheck("transaction rollback", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    db.exec('CREATE TABLE t(id INTEGER PRIMARY KEY)');
    var a = (db.isTransaction === false);
    db.exec('BEGIN');
    var b = (db.isTransaction === true);
    db.prepare('INSERT INTO t(id) VALUES (?)').run(1);
    db.exec('ROLLBACK');
    var c = (db.isTransaction === false);
    var n = db.prepare('SELECT COUNT(*) AS c FROM t').get().c;
    db.close();
    return a && b && c && n === 0;
})()
)JS");

    // 4. 语句可重复执行(run 自动 reset + 清绑定)
    runCheck("statement reuse", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    db.exec('CREATE TABLE t(id INTEGER)');
    var ins = db.prepare('INSERT INTO t(id) VALUES (?)');
    ins.run(1); ins.run(2); ins.run(3);
    var n = db.prepare('SELECT COUNT(*) AS c FROM t').get().c;
    var m = ins.run(4).lastInsertRowid;   // rowid: 无 INTEGER PRIMARY KEY 时按 rowid 递增
    db.close();
    return n === 3 && m === 4;
})()
)JS");

    // 5. columns() 与 iterate()
    runCheck("columns + iterate", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    db.exec('CREATE TABLE t(a INTEGER, b TEXT)');
    var cols = db.prepare('SELECT a, b FROM t').columns();
    var ins = db.prepare('INSERT INTO t(a, b) VALUES (?, ?)');
    ins.run(1, 'x'); ins.run(2, 'y'); ins.run(3, 'z');
    var out = [];
    var it = db.prepare('SELECT a FROM t ORDER BY a').iterate();
    var step = it.next();
    while (!step.done) { out.push(step.value.a); step = it.next(); }
    db.close();
    return cols.length === 2 && cols[0] === 'a' && cols[1] === 'b'
        && out.length === 3 && out[0] === 1 && out[2] === 3;
})()
)JS");

    // 6. 错误处理: 非法 SQL 抛 Error; close() 之后再用抛 Error
    runCheck("error handling", R"JS(
(function () {
    var db = new DatabaseSync(':memory:');
    var bad = false;
    try { db.exec('THIS IS NOT SQL'); } catch (e) { bad = (e instanceof Error); }
    db.exec('CREATE TABLE t(id INTEGER)');
    var st = db.prepare('SELECT * FROM t');
    db.close();
    var closed = false;
    try { st.all(); } catch (e) { closed = (e instanceof Error); }
    return bad && closed && db.isOpen === false;
})()
)JS");

    // 7. 文件库: 写入后重开验证持久化
    engine.eval("globalThis.__dbPath = '" + tmpDb + "';");
    runCheck("file db persistence", R"JS(
(function () {
    var db = new DatabaseSync(__dbPath);
    db.exec('CREATE TABLE IF NOT EXISTS t(v TEXT)');
    db.prepare('INSERT INTO t(v) VALUES (?)').run('persist');
    db.close();

    var db2 = new DatabaseSync(__dbPath);
    var v = db2.prepare('SELECT v FROM t').get().v;
    var tables = db2.prepare("SELECT COUNT(*) AS c FROM sqlite_master WHERE type = ?").get('table').c;
    var ro = false;
    try { new DatabaseSync(__dbPath, { readOnly: true }); ro = true; } catch (e) { ro = false; }
    db2.close();
    return v === 'persist' && tables === 1 && ro;
})()
)JS");

    sqliteModule.deinitEngine();
    remove(tmpDb.c_str());

    if (g_fail == 0)
        jzDebug() << "[test_jssqlite] ALL PASS";
    else
        jzDebug() << "[test_jssqlite] FAILED:" << g_fail;

    return g_fail == 0 ? 0 : 1;
}
