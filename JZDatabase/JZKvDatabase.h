#ifndef JZ_KV_DATABASE_H_
#define JZ_KV_DATABASE_H_
#include <memory>
#include <string>
#include <map>

#include "JZDataStream.h"

struct sqlite3;   // 前向声明: 仅持指针, 完整定义由 .cpp 的 sqlite3.h 提供

struct sqlite3;   // 前向声明: 不把 sqlite3.h 暴露给使用方(参照 TaskHistoryManager.h)

// ============================================================================
// 值类型: 抽象基类(镜像 JZNetPack 惯例)
//
// KV 存的值可能很重(如整包网络包), 因此统一用 shared_ptr 传递以避免拷贝。
// 子类需:
//   - dataType()       返回全局唯一类型号
//   - saveToStream()   通过 JZDataStream 序列化自身
//   - loadFromStream() 通过 JZDataStream 反序列化自身
// 并到 JZKvDataManager 注册, get() 才能按类型号重建出正确子类。
// ============================================================================
class JZFRAMEWORK_API JZKvAbstractData {
public:
    virtual ~JZKvAbstractData() {}
    virtual int dataType() const = 0;
    virtual void saveToStream(JZDataStream &s) const = 0;
    virtual void loadFromStream(JZDataStream &s) = 0;
};

typedef std::shared_ptr<JZKvAbstractData> JZKvDataPtr;

// ============================================================================
// 类型注册表: 让 get() 能按类型号重建正确子类(仿 JZNetPackManager)
// ============================================================================
class JZFRAMEWORK_API JZKvDataManager {
public:
    typedef JZKvDataPtr (*CreateFunc)();

    static JZKvDataManager *instance();

    template<class T>
    void registData(int type) { m_factory[type] = &create<T>; }

    bool isRegistered(int type) const;
    JZKvDataPtr createData(int type);   // 未知类型返回空指针

private:
    template<class T>
    static JZKvDataPtr create() { return JZKvDataPtr(new T()); }

    std::map<int, CreateFunc> m_factory;
};

// ============================================================================
// KV 数据库: 基于 SQLite 的薄封装
//
// schema:  kv(key TEXT PRIMARY KEY, data BLOB)
// 存储格式: BLOB = [int32 type][saveToStream 负载]
// 序列化经 JZDataStream 落到 JZByteArray, 再以 BLOB 写入。
// 先 open() 再读写; 句柄独占(不可拷贝), 生命周期由打开者负责。
// ============================================================================
class JZFRAMEWORK_API JZKvDatabase {
public:
    JZKvDatabase();
    ~JZKvDatabase();

    bool open(const std::string &path);     // 文件库(不存在则创建); 失败返回 false
    bool openInMemory();                    // :memory: 内存库(进程内有效)
    void close();
    bool isOpen() const { return m_db != nullptr; }

    bool set(const std::string &key, JZKvDataPtr value);  // value 为空视为删除
    JZKvDataPtr get(const std::string &key);              // 不存在/反序列化失败返回 nullptr
    bool remove(const std::string &key);
    bool contains(const std::string &key) const;

private:
    JZ_DISABLE_COPY(JZKvDatabase);
    bool createTable();

    sqlite3 *m_db = nullptr;
};

#endif // !JZ_KV_DATABASE_H_
