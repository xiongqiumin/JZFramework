#include "JZSqlDatabase.h"

#include "sqlite3.h"

// 说明: sqlite3.h **只**在这个 .cpp 里出现 —— 头文件只前向声明, 使用方(框架内外)
// 都不需要看见 sqlite 的 C API。详见 JZSqlDatabase.h 顶部。

// ==================== JZSqlDatabase ====================
JZSqlDatabase::JZSqlDatabase()
{
}

JZSqlDatabase::~JZSqlDatabase()
{
    close();
}

void JZSqlDatabase::setError(const std::string &what)
{
    if (m_db)
    {
        const char *msg = sqlite3_errmsg(m_db);
        m_error = what + (msg ? (std::string(": ") + msg) : std::string());
    }
    else
    {
        m_error = what;
    }
}

bool JZSqlDatabase::open(const std::string &path)
{
    close();

    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK)
    {
        // 打开失败不致命: 由调用方决定降级策略(见头文件说明)
        m_db = db;                    // 失败时句柄可能非空, 借它取 errmsg
        setError("open failed: " + path);
        if (db)
            sqlite3_close_v2(db);
        m_db = nullptr;
        return false;
    }

    m_db = db;
    m_path = path;
    m_error.clear();
    return true;
}

bool JZSqlDatabase::openInMemory()
{
    return open(":memory:");
}

void JZSqlDatabase::close()
{
    if (m_db)
    {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
    m_path.clear();
}

bool JZSqlDatabase::exec(const std::string &sql)
{
    if (!m_db)
    {
        m_error = "exec on a closed database";
        return false;
    }

    // sqlite3_exec 支持分号分隔的多条语句(建表 + 建索引 + 事务都靠它)
    char *err = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
    {
        m_error = err ? err : "exec failed";
        if (err)
            sqlite3_free(err);
        return false;
    }

    m_error.clear();
    return true;
}

int64_t JZSqlDatabase::lastInsertRowId() const
{
    return m_db ? (int64_t)sqlite3_last_insert_rowid(m_db) : 0;
}

int JZSqlDatabase::changes() const
{
    return m_db ? sqlite3_changes(m_db) : 0;
}

void JZSqlDatabase::setBusyTimeout(int ms)
{
    if (m_db)
        sqlite3_busy_timeout(m_db, ms);
}

// ==================== JZSqlQuery ====================
JZSqlQuery::JZSqlQuery()
{
}

JZSqlQuery::~JZSqlQuery()
{
    finalize();
}

void JZSqlQuery::setError(const std::string &what)
{
    m_error = what;
}

bool JZSqlQuery::prepare(const JZSqlDatabase &db, const std::string &sql)
{
    finalize();
    if (!db.isOpen())
    {
        setError("prepare on a closed database");
        return false;
    }

    sqlite3_stmt *stmt = nullptr;
    int rc = sqlite3_prepare_v2(db.m_db, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        setError(std::string("prepare failed: ") + sqlite3_errmsg(db.m_db));
        if (stmt)
            sqlite3_finalize(stmt);
        return false;
    }

    m_stmt = stmt;
    m_stepDone = false;
    m_error.clear();
    return true;
}

void JZSqlQuery::reset()
{
    if (!m_stmt)
        return;
    sqlite3_reset(m_stmt);
    m_stepDone = false;
}

void JZSqlQuery::clearBindings()
{
    if (!m_stmt)
        return;
    sqlite3_clear_bindings(m_stmt);
}

void JZSqlQuery::finalize()
{
    if (m_stmt)
    {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
    m_stepDone = false;
}

bool JZSqlQuery::bind(int index, int value)
{
    if (!m_stmt)
        return false;
    return sqlite3_bind_int(m_stmt, index, value) == SQLITE_OK;
}

bool JZSqlQuery::bind(int index, int64_t value)
{
    if (!m_stmt)
        return false;
    return sqlite3_bind_int64(m_stmt, index, value) == SQLITE_OK;
}

bool JZSqlQuery::bind(int index, double value)
{
    if (!m_stmt)
        return false;
    return sqlite3_bind_double(m_stmt, index, value) == SQLITE_OK;
}

bool JZSqlQuery::bind(int index, const std::string &value)
{
    if (!m_stmt)
        return false;
    // SQLITE_TRANSIENT: 让 sqlite 自己拷一份 ⇒ 调用方不必保证 buffer 活到 step()
    return sqlite3_bind_text(m_stmt, index, value.c_str(), (int)value.size(),
                             SQLITE_TRANSIENT) == SQLITE_OK;
}

bool JZSqlQuery::bind(int index, const void *data, int size)
{
    if (!m_stmt)
        return false;
    return sqlite3_bind_blob(m_stmt, index, data, size, SQLITE_TRANSIENT) == SQLITE_OK;
}

bool JZSqlQuery::bindNull(int index)
{
    if (!m_stmt)
        return false;
    return sqlite3_bind_null(m_stmt, index) == SQLITE_OK;
}

bool JZSqlQuery::step()
{
    if (!m_stmt)
    {
        setError("step on an unprepared query");
        return false;
    }

    int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW)
    {
        m_stepDone = false;
        return true;
    }

    // SQLITE_DONE = 正常结束; 其它都是错
    m_stepDone = (rc == SQLITE_DONE);
    if (!m_stepDone)
    {
        m_error = std::string("step failed (rc=") + std::to_string(rc) + ")";
    }
    return false;
}

int JZSqlQuery::columnCount() const
{
    return m_stmt ? sqlite3_column_count(m_stmt) : 0;
}

int JZSqlQuery::columnInt(int index) const
{
    return m_stmt ? sqlite3_column_int(m_stmt, index) : 0;
}

int64_t JZSqlQuery::columnInt64(int index) const
{
    return m_stmt ? (int64_t)sqlite3_column_int64(m_stmt, index) : 0;
}

double JZSqlQuery::columnDouble(int index) const
{
    return m_stmt ? sqlite3_column_double(m_stmt, index) : 0.0;
}

std::string JZSqlQuery::columnText(int index) const
{
    if (!m_stmt)
        return std::string();
    const unsigned char *p = sqlite3_column_text(m_stmt, index);
    if (!p)
        return std::string();
    // 用 column_bytes 而不是 c_str(): 文本里可能含 '\0'(与旧代码的 colText 同口径)
    return std::string((const char *)p, (size_t)sqlite3_column_bytes(m_stmt, index));
}

bool JZSqlQuery::columnIsNull(int index) const
{
    return !m_stmt || sqlite3_column_type(m_stmt, index) == SQLITE_NULL;
}

const void *JZSqlQuery::columnBlob(int index, int *sizeOut) const
{
    if (sizeOut)
        *sizeOut = 0;
    if (!m_stmt)
        return nullptr;

    const void *p = sqlite3_column_blob(m_stmt, index);
    int n = sqlite3_column_bytes(m_stmt, index);
    if (sizeOut)
        *sizeOut = n;
    return p;
}
