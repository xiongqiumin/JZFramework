#ifndef JZ_SQL_DATABASE_H_
#define JZ_SQL_DATABASE_H_
// ============================================================================
// JZSqlDatabase / JZSqlQuery —— SQLite 的 C++ 封装
//
// 为什么要有这一层(而不是各处直接 include "sqlite3.h"):
//   · **边界整洁**: sqlite3 的 C API 是"第三方原始接口"(sqlite3_open_v2 /
//     sqlite3_prepare_v2 / bind / step / column / finalize...), 谁用它谁就得
//     include 那个 9MB amalgamation 的头, 还要自己管 prepare/finalize、
//     SQLITE_TRANSIENT/STATIC、错误码与 errmsg。框架的使用方(如 code_agent 的任务库、
//     BTE 的记录库)只想要"打开库 → 跑 SQL → 取值"。
//   · **动态库友好**: sqlite3.c 编在 JZFramework 里; 若把 sqlite3 的这些符号当成
//     "要跨 DLL 边界调用的第三方 C 函数", 就得把几百个 sqlite3_* 全导出(SQLITE_API)。
//     封成两个 JZFRAMEWORK_API 类之后, 使用方**一个 sqlite 符号都不碰**,
//     dllexport 面就只有这两个类。
//
// 用法(与之前手写的 sqlite 代码一一对应):
//     JZSqlDatabase db;
//     if (!db.open("/path/tasks.db")) { ...db.lastError()... }
//     db.setBusyTimeout(3000);
//     db.exec("CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, name TEXT);");
//
//     JZSqlQuery q;
//     q.prepare(db, "INSERT OR REPLACE INTO t(id, name) VALUES(?, ?)");
//     q.bind(1, 7);
//     q.bind(2, std::string("hello"));
//     q.step();
//
//     JZSqlQuery sel;
//     sel.prepare(db, "SELECT id, name FROM t ORDER BY id");
//     while (sel.step()) { int id = sel.columnInt(0); std::string n = sel.columnText(1); }
//     if (!sel.isDone()) ... // step() 返回 false 且 isDone() 为假 = 真出错(见 lastError())
//
// 约定:
//   · 绑定下标从 **1** 开始, 列下标从 **0** 开始(sqlite 原生口径)。
//   · 文本/二进制一律**拷贝**(SQLITE_TRANSIENT): 调用方不必保证 buffer 活到 step()。
//   · 两个类都不可拷贝(句柄独占); 析构会自动 close/finalize。
// ============================================================================

#include <stdint.h>

#include <string>

#include "JZGlobal.h"

struct sqlite3;        // 前向声明: 使用方不需要 sqlite3.h
struct sqlite3_stmt;

class JZSqlQuery;

// ----------------------------------------------------------------------------
// 一个数据库连接(RAII)。
// 打开失败不抛异常: isOpen()==false + lastError() 给出原因, 由调用方决定怎么降级
// (例如任务库打不开就退化成"仅内存可用", 界面照常)。
// ----------------------------------------------------------------------------
class JZFRAMEWORK_API JZSqlDatabase
{
public:
    JZSqlDatabase();
    ~JZSqlDatabase();
    JZ_DISABLE_COPY(JZSqlDatabase)

    // 打开文件库(不存在则创建); 失败返回 false
    bool open(const std::string &path);
    // 打开内存库(":memory:", 进程内有效)
    bool openInMemory();
    // 关闭; 可重复调用
    void close();

    bool isOpen() const { return m_db != nullptr; }
    const std::string &path() const { return m_path; }

    // 执行 SQL: 可以含多条语句(建表/索引/事务 "begin;"/"commit;" 都走它)
    bool exec(const std::string &sql);

    // 最近一次失败的原因(空串 = 没出错)
    const std::string &lastError() const { return m_error; }

    // 最近一次 INSERT 的自增主键 / 最近一次改动行数
    int64_t lastInsertRowId() const;
    int     changes() const;

    // 遇到别的连接持锁时等待多久再失败(默认 sqlite 是立即失败); <=0 = 不等
    void setBusyTimeout(int ms);

private:
    void setError(const std::string &what);

    sqlite3    *m_db = nullptr;
    std::string m_path;
    std::string m_error;

    friend class JZSqlQuery;      // 语句要拿 m_db
};

// ----------------------------------------------------------------------------
// 一条预编译语句(RAII): prepare -> bind -> step -> 取值; 析构自动 finalize。
// 同一条 SQL 反复用: reset() 复位游标(SQLite 语义: 不清绑定), clearBindings() 清绑定。
// ----------------------------------------------------------------------------
class JZFRAMEWORK_API JZSqlQuery
{
public:
    JZSqlQuery();
    ~JZSqlQuery();
    JZ_DISABLE_COPY(JZSqlQuery)

    // 预编译; 失败返回 false, 原因见 db.lastError() / lastError()
    // 库参数收 const 引用: 预编译不改连接状态, 这样 const 成员函数里也能直接查(不必 const_cast)
    bool prepare(const JZSqlDatabase &db, const std::string &sql);
    // SQLite 语义: 复位游标以便再次 step(**不清**绑定)
    void reset();
    // 清掉所有绑定(配合 reset() 复用同一条 SQL)
    void clearBindings();
    void finalize();
    bool isValid() const { return m_stmt != nullptr; }

    // 绑定(下标 1 起); 越界/类型不符返回 false
    bool bind(int index, int value);
    bool bind(int index, int64_t value);
    bool bind(int index, double value);
    bool bind(int index, const std::string &value);          // 文本(拷贝)
    bool bind(int index, const void *data, int size);        // 二进制(拷贝)
    bool bindNull(int index);

    // 步进: true = 取到一行; false = 结束**或**出错 —— 用 isDone() 区分
    bool step();
    bool isDone() const { return m_stepDone; }

    // 取值(列下标 0 起; 越界返回 0/空)
    int         columnCount() const;
    int         columnInt(int index) const;
    int64_t     columnInt64(int index) const;
    double      columnDouble(int index) const;
    std::string columnText(int index) const;                 // NULL 返回空串
    bool        columnIsNull(int index) const;
    // 二进制列: 返回内部指针(有效到下次 step/reset/finalize), sizeOut 回长度
    const void *columnBlob(int index, int *sizeOut) const;

    const std::string &lastError() const { return m_error; }

private:
    void setError(const std::string &what);

    sqlite3_stmt *m_stmt = nullptr;
    bool          m_stepDone = false;
    std::string   m_error;
};

#endif // !JZ_SQL_DATABASE_H_
