#include "JZJSqliteModule.h"

#include <cmath>

#include "JZJSBind.h"

// ======================================================================
// JZJSSqliteDb: 一个打开的数据库连接(JS 包装对象由 GC 拥有)
// ======================================================================
JZJSSqliteDb::JZJSSqliteDb()
{
    engine = nullptr;
    module = nullptr;
    db = nullptr;
    nextStmtId = 1;
    closed = false;
}

JZJSSqliteDb::~JZJSSqliteDb()
{
    // GC 回收(或引擎销毁)时兜底: 先全部 finalize 语句, 再关连接。
    for (std::map<int, sqlite3_stmt*>::iterator it = stmts.begin(); it != stmts.end(); ++it)
    {
        if (it->second)
            sqlite3_finalize(it->second);
    }
    stmts.clear();

    if (db)
    {
        sqlite3_close_v2(db);
        db = nullptr;
    }
}

// ======================================================================
// JZJSqliteModule
// ======================================================================
JZJSqliteModule::JZJSqliteModule()
{
}

JZJSqliteModule::~JZJSqliteModule()
{
    deinitEngine();
}

JSValue JZJSqliteModule::throwError(const std::string& msg)
{
    JSContext* ctx = m_engine->context();
    JSValue err = JS_NewError(ctx);
    JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.c_str()));
    // JS_Throw 消费 err 的所有权, 并返回 JS_EXCEPTION
    return JS_Throw(ctx, err);
}

void JZJSqliteModule::initEngine(JZJSEngine* engine)
{
    m_engine = engine;
    JSContext* ctx = m_engine->context();
    auto global = m_engine->globalObject();

    // ---- 原生类 JZJSSqliteDb ----
    // 用 jsbind 绑定成 JS 类: 连接对象由 GC 拥有, 被回收时自动 close(不依赖用户显式调用)。
    JZJSqliteModule* self = this;

    jsbind::ClassBind<JZJSSqliteDb> bind_db("JZJSSqliteDb");
    bind_db.def("exec", [self](JZJSSqliteDb* o, const std::string& sql) -> JSValue
                        { return self->dbExec(o, sql); });
    bind_db.def("prepare", [self](JZJSSqliteDb* o, const std::string& sql) -> JSValue
                           { return self->dbPrepare(o, sql); });
    bind_db.def("close", [self](JZJSSqliteDb* o)
                         { self->dbClose(o); });

    bind_db.def("stmtColumns", [self](JZJSSqliteDb* o, int id) -> JSValue
                               { return self->stmtColumns(o, id); });
    bind_db.def("stmtRun", [self](JZJSSqliteDb* o, int id, JSValue anon, JSValue named) -> JSValue
                           { return self->stmtRun(o, id, anon, named); });
    bind_db.def("stmtGet", [self](JZJSSqliteDb* o, int id, JSValue anon, JSValue named) -> JSValue
                           { return self->stmtGet(o, id, anon, named); });
    bind_db.def("stmtAll", [self](JZJSSqliteDb* o, int id, JSValue anon, JSValue named) -> JSValue
                           { return self->stmtAll(o, id, anon, named); });
    bind_db.def("stmtFinalize", [self](JZJSSqliteDb* o, int id)
                                { self->stmtFinalize(o, id); });

    bind_db.defProperty<std::string>("path",
        [](JZJSSqliteDb* o) { return o->path; }, nullptr);
    bind_db.defProperty<bool>("isOpen",
        [](JZJSSqliteDb* o) { return o->db != nullptr && !o->closed; }, nullptr);
    bind_db.defProperty<bool>("isTransaction",
        [](JZJSSqliteDb* o) { return o->db != nullptr && !sqlite3_get_autocommit(o->db); }, nullptr);

    m_engine->registClass(bind_db.classDefine());
    m_typeDb = m_engine->ctypeidToType(typeid(JZJSSqliteDb).name());

    // ---- __jz.sqlite 命名空间(复用已存在的 __jz, 不覆盖其它子命名空间) ----
    JSValue jz = m_engine->getProperty(global, "__jz");
    bool jzFromGet = !JS_IsUndefined(jz);
    if (!jzFromGet)
    {
        jz = m_engine->createObject();
        m_engine->setProperty(global, "__jz", jz);
    }

    JSValue ns = m_engine->createObject();
    m_engine->registFunction(ns, "openDb",
                             jsbind::createSelfFunction(this, &JZJSqliteModule::openDb));
    m_engine->registFunction(ns, "version",
                             jsbind::createSelfFunction(this, &JZJSqliteModule::sqliteVersion));
    m_engine->setProperty(jz, "sqlite", ns);

    if (jzFromGet)
        JS_FreeValue(ctx, jz);   // 释放 getProperty 多 dup 的引用

    // JS 适配层(dist/index.js 的 DatabaseSync/StatementSync)由用户入口模块
    // `import { DatabaseSync } from './sqlite/dist/index.js'` 被动加载, 本函数只注册原子 API。
}

void JZJSqliteModule::deinitEngine()
{
    // 连接对象由 JS GC 拥有, 随 ctx 释放自动析构(析构里做 finalize + close)。
    m_engine = nullptr;
    m_typeDb = -1;
}

// ======================================================================
// 原子 API
// ======================================================================
JSValue JZJSqliteModule::sqliteVersion()
{
    return JS_NewString(m_engine->context(), sqlite3_libversion());
}

JSValue JZJSqliteModule::openDb(const std::string& path, const Json::Value& options)
{
    JSContext* ctx = m_engine->context();

    bool readOnly = options.isMember("readOnly") && options["readOnly"].asBool();
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    flags |= SQLITE_OPEN_URI;

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        std::string msg = db ? sqlite3_errmsg(db) : std::string("unable to open database file");
        if (db)
            sqlite3_close_v2(db);
        return throwError("unable to open database " + path + ": " + msg);
    }

    // Node 的 DatabaseSync 默认开启外键约束(Node 亦如此), 这里保持一致
    bool fk = true;
    if (options.isMember("enableForeignKeyConstraints"))
        fk = options["enableForeignKeyConstraints"].asBool();
    if (fk)
        sqlite3_exec(db, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);

    if (options.isMember("timeout"))
    {
        int t = options["timeout"].asInt();
        if (t > 0)
            sqlite3_busy_timeout(db, t);
    }

    JZJSSqliteDb* obj = new JZJSSqliteDb();
    obj->engine = m_engine;
    obj->module = this;
    obj->db = db;
    obj->path = path;

    return m_engine->createValue(m_typeDb, obj);
}

JSValue JZJSqliteModule::dbExec(JZJSSqliteDb* o, const std::string& sql)
{
    if (!o->db || o->closed)
        return throwError("The database connection is not open");

    char* err = nullptr;
    int rc = sqlite3_exec(o->db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string msg = err ? std::string(err) : std::string(sqlite3_errmsg(o->db));
        if (err)
            sqlite3_free(err);
        return throwError(msg);
    }
    return JS_UNDEFINED;
}

JSValue JZJSqliteModule::dbPrepare(JZJSSqliteDb* o, const std::string& sql)
{
    if (!o->db || o->closed)
        return throwError("The database connection is not open");

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(o->db, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK || stmt == nullptr)
    {
        std::string msg = sqlite3_errmsg(o->db);
        if (stmt)
            sqlite3_finalize(stmt);
        return throwError(msg);
    }

    int id = o->nextStmtId++;
    o->stmts[id] = stmt;
    return JS_NewInt32(m_engine->context(), id);
}

void JZJSqliteModule::dbClose(JZJSSqliteDb* o)
{
    if (o->closed)
        return;

    // 先释放本连接所有的语句, 再关连接(sqlite3_close_v2 允许有未 finalize 的语句,
    // 但显式释放更干净, 也能让 JS 侧持有的 stmtId 立刻失效)
    for (std::map<int, sqlite3_stmt*>::iterator it = o->stmts.begin(); it != o->stmts.end(); ++it)
    {
        if (it->second)
            sqlite3_finalize(it->second);
    }
    o->stmts.clear();

    if (o->db)
    {
        sqlite3_close_v2(o->db);
        o->db = nullptr;
    }
    o->closed = true;
}

sqlite3_stmt* JZJSqliteModule::findStmt(JZJSSqliteDb* o, int stmtId)
{
    if (!o || o->closed || !o->db)
        return nullptr;
    std::map<int, sqlite3_stmt*>::iterator it = o->stmts.find(stmtId);
    return (it == o->stmts.end()) ? nullptr : it->second;
}

// ======================================================================
// 参数绑定
// ======================================================================
bool JZJSqliteModule::bindOne(sqlite3_stmt* stmt, int index, JSValue v)
{
    JSContext* ctx = m_engine->context();

    if (JS_IsNull(v) || JS_IsUndefined(v))
    {
        sqlite3_bind_null(stmt, index);
        return true;
    }
    if (JS_IsBool(v))
    {
        sqlite3_bind_int(stmt, index, JS_ToBool(ctx, v) > 0 ? 1 : 0);
        return true;
    }
    if (JS_IsString(v))
    {
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, v);
        if (!s)
            return false;
        sqlite3_bind_text(stmt, index, s, (int)len, SQLITE_TRANSIENT);
        JS_FreeCString(ctx, s);
        return true;
    }
    // Uint8Array / ArrayBuffer -> BLOB
    {
        size_t size = 0;
        uint8_t* buf = jzUint8ArrayOrNull(ctx, &size, v);
        if (buf)
        {
            sqlite3_bind_blob(stmt, index, buf, (int)size, SQLITE_TRANSIENT);
            return true;
        }
    }
    if (JS_IsArrayBuffer(v))
    {
        size_t size = 0;
        uint8_t* buf = JS_GetArrayBuffer(ctx, &size, v);
        if (buf)
        {
            sqlite3_bind_blob(stmt, index, buf, (int)size, SQLITE_TRANSIENT);
            return true;
        }
    }
    if (JS_IsNumber(v) || JS_IsBigInt(v))
    {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) != 0)
            return false;
        // 整数值用 INTEGER 存(保留 id/rowid 的整数语义), 其余用 REAL
        if (std::floor(d) == d && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
            sqlite3_bind_int64(stmt, index, (sqlite3_int64)d);
        else
            sqlite3_bind_double(stmt, index, d);
        return true;
    }
    return false;
}

bool JZJSqliteModule::bindParams(JZJSSqliteDb* o, sqlite3_stmt* stmt,
                                 JSValue anon, JSValue named, std::string& err)
{
    JSContext* ctx = m_engine->context();
    bool namedOk = JS_IsObject(named);
    bool anonOk = JS_IsArray(anon);

    int count = sqlite3_bind_parameter_count(stmt);
    uint32_t anonIdx = 0;

    for (int i = 1; i <= count; i++)
    {
        const char* name = sqlite3_bind_parameter_name(stmt, i);
        JSValue v = JS_UNDEFINED;
        bool found = false;

        if (name && name[0] && namedOk)
        {
            // 具名参数: 先按完整键名(:name)找, 再按去前缀的裸名(name)找 —— 对齐 Node 的
            // allowBareNamedParameters 默认行为(statement.run({ name: 1 }) 可绑 :name)。
            JSValue full = JS_GetPropertyStr(ctx, named, name);
            if (!JS_IsUndefined(full))
            {
                v = full;
                found = true;
            }
            else
            {
                JS_FreeValue(ctx, full);
                JSValue bare = JS_GetPropertyStr(ctx, named, name + 1);
                if (!JS_IsUndefined(bare))
                {
                    v = bare;
                    found = true;
                }
                else
                {
                    JS_FreeValue(ctx, bare);
                }
            }
        }
        else if (!name && anonOk)
        {
            // 位置参数: 按出现顺序取
            JSValue item = JS_GetPropertyUint32(ctx, anon, anonIdx++);
            if (!JS_IsUndefined(item))
            {
                v = item;
                found = true;
            }
            else
            {
                JS_FreeValue(ctx, item);
            }
        }

        if (found)
        {
            bool ok = bindOne(stmt, i, v);
            JS_FreeValue(ctx, v);
            if (!ok)
            {
                err = "unsupported parameter type for parameter " + std::to_string(i);
                return false;
            }
        }
        else
        {
            sqlite3_bind_null(stmt, i);   // 未提供 -> NULL(Node 会报错, 这里放宽为 NULL)
        }
    }
    return true;
}

// ======================================================================
// 结果集 -> JS
// ======================================================================
JSValue JZJSqliteModule::columnToJs(JZJSSqliteDb* o, sqlite3_stmt* stmt, int col)
{
    JSContext* ctx = m_engine->context();
    switch (sqlite3_column_type(stmt, col))
    {
    case SQLITE_INTEGER:
        return JS_NewInt64(ctx, sqlite3_column_int64(stmt, col));
    case SQLITE_FLOAT:
        return JS_NewFloat64(ctx, sqlite3_column_double(stmt, col));
    case SQLITE_TEXT:
    {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        int n = sqlite3_column_bytes(stmt, col);
        return JS_NewStringLen(ctx, (const char*)t, (size_t)n);
    }
    case SQLITE_BLOB:
    {
        const void* b = sqlite3_column_blob(stmt, col);
        int n = sqlite3_column_bytes(stmt, col);
        return JS_NewUint8ArrayCopy(ctx, (const uint8_t*)b, (size_t)n);
    }
    default:
        return JS_NULL;
    }
}

JSValue JZJSqliteModule::rowToObject(JZJSSqliteDb* o, sqlite3_stmt* stmt)
{
    JSContext* ctx = m_engine->context();
    JSValue obj = JS_NewObject(ctx);

    int n = sqlite3_column_count(stmt);
    for (int i = 0; i < n; i++)
    {
        const char* name = sqlite3_column_name(stmt, i);
        JSValue v = columnToJs(o, stmt, i);
        if (name && name[0])
            JS_SetPropertyStr(ctx, obj, name, v);
        else
            JS_SetPropertyStr(ctx, obj, ("column" + std::to_string(i)).c_str(), v);
    }
    return obj;
}

// ======================================================================
// 语句 API
// ======================================================================
JSValue JZJSqliteModule::stmtColumns(JZJSSqliteDb* o, int stmtId)
{
    sqlite3_stmt* stmt = findStmt(o, stmtId);
    if (!stmt)
        return throwError("statement has been finalized");

    JSContext* ctx = m_engine->context();
    int n = sqlite3_column_count(stmt);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, sqlite3_column_name(stmt, i)));
    return arr;
}

JSValue JZJSqliteModule::stmtRun(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named)
{
    sqlite3_stmt* stmt = findStmt(o, stmtId);
    if (!stmt)
        return throwError("statement has been finalized");

    JSContext* ctx = m_engine->context();

    std::string err;
    if (!bindParams(o, stmt, anon, named, err))
        return throwError(err);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        return throwError(sqlite3_errmsg(o->db));

    JSValue res = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, res, "changes", JS_NewInt32(ctx, sqlite3_changes(o->db)));
    JS_SetPropertyStr(ctx, res, "lastInsertRowid",
                      JS_NewInt64(ctx, sqlite3_last_insert_rowid(o->db)));
    return res;
}

JSValue JZJSqliteModule::stmtGet(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named)
{
    sqlite3_stmt* stmt = findStmt(o, stmtId);
    if (!stmt)
        return throwError("statement has been finalized");

    std::string err;
    if (!bindParams(o, stmt, anon, named, err))
        return throwError(err);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        JSValue row = rowToObject(o, stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return row;
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (rc != SQLITE_DONE)
        return throwError(sqlite3_errmsg(o->db));
    return JS_NULL;
}

JSValue JZJSqliteModule::stmtAll(JZJSSqliteDb* o, int stmtId, JSValue anon, JSValue named)
{
    sqlite3_stmt* stmt = findStmt(o, stmtId);
    if (!stmt)
        return throwError("statement has been finalized");

    JSContext* ctx = m_engine->context();

    std::string err;
    if (!bindParams(o, stmt, anon, named, err))
        return throwError(err);

    JSValue arr = JS_NewArray(ctx);
    uint32_t i = 0;
    int rc = SQLITE_OK;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        JS_SetPropertyUint32(ctx, arr, i++, rowToObject(o, stmt));

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    if (rc != SQLITE_DONE)
    {
        JS_FreeValue(ctx, arr);
        return throwError(sqlite3_errmsg(o->db));
    }
    return arr;
}

void JZJSqliteModule::stmtFinalize(JZJSSqliteDb* o, int stmtId)
{
    if (!o)
        return;
    std::map<int, sqlite3_stmt*>::iterator it = o->stmts.find(stmtId);
    if (it == o->stmts.end())
        return;
    if (it->second)
        sqlite3_finalize(it->second);
    o->stmts.erase(it);
}
