#include "JZKvDatabase.h"

#include "JZDebug.h"
#include "sqlite3.h"

// ==================== JZKvDataManager ====================
JZKvDataManager *JZKvDataManager::instance()
{
    static JZKvDataManager inst;
    return &inst;
}

bool JZKvDataManager::isRegistered(int type) const
{
    return m_factory.find(type) != m_factory.end();
}

JZKvDataPtr JZKvDataManager::createData(int type)
{
    auto it = m_factory.find(type);
    if (it == m_factory.end())
        return JZKvDataPtr();
    return it->second();
}

// ==================== JZKvDatabase ====================
namespace
{
// 语句 RAII: 仅配对 prepare/finalize, 不引入查询抽象(参照 TaskHistoryManager)。
class Stmt {
public:
    Stmt(sqlite3 *db, const char *sql)
    {
        m_rc = sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr);
    }
    ~Stmt()
    {
        if (m_stmt)
            sqlite3_finalize(m_stmt);
    }

    Stmt(const Stmt &) = delete;
    Stmt &operator=(const Stmt &) = delete;

    sqlite3_stmt *get() const { return m_stmt; }
    bool ok() const { return m_rc == SQLITE_OK && m_stmt != nullptr; }

private:
    sqlite3_stmt *m_stmt = nullptr;
    int m_rc = SQLITE_ERROR;
};
} // namespace

JZKvDatabase::JZKvDatabase() {}

JZKvDatabase::~JZKvDatabase()
{
    close();
}

void JZKvDatabase::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool JZKvDatabase::createTable()
{
    if (!m_db)
        return false;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS kv ("
        "  key  TEXT PRIMARY KEY,"
        "  data BLOB NOT NULL);";

    char *err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        jzDebug() << "[JZKvDatabase] create table failed:" << (err ? err : "");
        if (err)
            sqlite3_free(err);
        return false;
    }
    return true;
}

bool JZKvDatabase::open(const std::string &path)
{
    close();
    if (path.empty())
        return false;

    int rc = sqlite3_open_v2(path.c_str(), &m_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        jzDebug() << "[JZKvDatabase] open failed:" << path
                  << (m_db ? sqlite3_errmsg(m_db) : "");
        if (m_db) {
            sqlite3_close_v2(m_db);
            m_db = nullptr;
        }
        return false;
    }

    sqlite3_busy_timeout(m_db, 3000);   // 多进程/被杀进程留下的锁, 短暂等待而非直接失败
    return createTable();
}

bool JZKvDatabase::openInMemory()
{
    return open(":memory:");
}

bool JZKvDatabase::set(const std::string &key, JZKvDataPtr value)
{
    if (!m_db)
        return false;
    if (!value)
        return remove(key);   // 空指针视为删除

    // 序列化: [int32 type][saveToStream 负载]
    JZByteArray buf;
    {
        JZDataStream s(&buf, JZ::WriteOnly);
        s << (int32_t)value->dataType();
        value->saveToStream(s);
        if (s.isError())
            return false;
    }

    Stmt st(m_db, "INSERT OR REPLACE INTO kv(key,data) VALUES(?,?)");
    if (!st.ok())
        return false;
    sqlite3_bind_text(st.get(), 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);
    sqlite3_bind_blob(st.get(), 2, buf.data(), buf.size(), SQLITE_TRANSIENT);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

JZKvDataPtr JZKvDatabase::get(const std::string &key)
{
    if (!m_db)
        return JZKvDataPtr();

    Stmt st(m_db, "SELECT data FROM kv WHERE key=?");
    if (!st.ok())
        return JZKvDataPtr();
    sqlite3_bind_text(st.get(), 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);

    JZKvDataPtr result;
    if (sqlite3_step(st.get()) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st.get(), 0);
        int len = sqlite3_column_bytes(st.get(), 0);
        if (blob && len > 0) {
            JZByteArray buf((const char *)blob, len);
            JZDataStream s(&buf, JZ::ReadOnly);
            int32_t type = 0;
            s >> type;
            JZKvDataPtr v = JZKvDataManager::instance()->createData(type);
            if (v && !s.isError()) {
                v->loadFromStream(s);
                if (!s.isError())
                    result = v;
            }
        }
    }
    return result;
}

bool JZKvDatabase::remove(const std::string &key)
{
    if (!m_db)
        return false;

    Stmt st(m_db, "DELETE FROM kv WHERE key=?");
    if (!st.ok())
        return false;
    sqlite3_bind_text(st.get(), 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);
    return sqlite3_step(st.get()) == SQLITE_DONE;
}

bool JZKvDatabase::contains(const std::string &key) const
{
    if (!m_db)
        return false;

    Stmt st(m_db, "SELECT 1 FROM kv WHERE key=? LIMIT 1");
    if (!st.ok())
        return false;
    sqlite3_bind_text(st.get(), 1, key.c_str(), (int)key.size(), SQLITE_TRANSIENT);
    return sqlite3_step(st.get()) == SQLITE_ROW;
}
