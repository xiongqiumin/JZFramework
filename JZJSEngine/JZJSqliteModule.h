#ifndef JS_ENGINE_SQLITE_MODULE_H_
#define JS_ENGINE_SQLITE_MODULE_H_

#include <map>
#include <string>

#include "JZJSEngine.h"
#include "sqlite3.h"

class JZJSqliteModule;

// 一个打开的数据库连接。
// 生命周期: 由 JS 侧的 DatabaseSync 对象拥有(jsbind ClassBind -> GC finalizer),
// 用户显式 close() 或对象被 GC 时析构; 析构会 finalize 它创建的所有语句再关闭连接。
struct JZJSSqliteDb
{
    JZJSSqliteDb();
    ~JZJSSqliteDb();

    JZJSEngine* engine;                       // 构造 JS 值用
    JZJSqliteModule* module;
    sqlite3* db;
    std::string path;
    std::map<int, sqlite3_stmt*> stmts;       // stmtId -> 预编译语句(由本连接拥有)
    int nextStmtId;
    bool closed;
};

// SQLite 数据库模块: 注册 __jz.sqlite 原子 API + 原生类 JZJSSqliteDb。
// 上层 Node 风格封装见 JZJSEngine/js_wrapper/sqlite/index.ts (DatabaseSync/StatementSync)。
class JZJSqliteModule : public JZObject
{
public:
    JZJSqliteModule();
    ~JZJSqliteModule();

    void initEngine(JZJSEngine* engine);
    void deinitEngine();

    // ---- 原子 API(挂在 __jz.sqlite 下) ----
    JSValue openDb(const std::string& path, const Json::Value& options);
    JSValue sqliteVersion();

    // 以下由 JZJSSqliteDb 的方法(lambda)转发调用(供 ClassBind 使用)
    JSValue dbExec(JZJSSqliteDb* o, const std::string& sql);
    JSValue dbPrepare(JZJSSqliteDb* o, const std::string& sql);   // 返回 stmtId(int) 或抛异常
    void dbClose(JZJSSqliteDb* o);

    JSValue stmtColumns(JZJSSqliteDb* o, int stmtId);
    JSValue stmtRun(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named);
    JSValue stmtGet(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named);
    JSValue stmtAll(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named);
    void stmtFinalize(JZJSSqliteDb* o, int stmtId);

    // ---- 内部工具 ----
    // 抛出 JS Error 并返回 JS_EXCEPTION(调用方直接 return)
    JSValue throwError(const std::string& msg);

protected:
    // 参数绑定: anon = 位置参数数组, named = 具名参数对象(键可带可不带 :/@/$ 前缀)
    bool bindParams(JZJSSqliteDb* o, sqlite3_stmt* stmt, JSValue anon, JSValue named, std::string& err);
    bool bindOne(sqlite3_stmt* stmt, int index, JSValue v);

    // 结果集 -> JS 对象/数组
    JSValue rowToObject(JZJSSqliteDb* o, sqlite3_stmt* stmt);
    JSValue columnToJs(JZJSSqliteDb* o, sqlite3_stmt* stmt, int col);

    sqlite3_stmt* findStmt(JZJSSqliteDb* o, int stmtId);

    JZJSEngine* m_engine = nullptr;
    int m_typeDb = -1;
};

#endif  // !JS_ENGINE_SQLITE_MODULE_H_
