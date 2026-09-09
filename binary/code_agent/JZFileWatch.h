#ifndef JZFILEWATCH_H
#define JZFILEWATCH_H

#include <string>
#include <list>
#include <unordered_map>
#include "JZMutex.h"
#include "JZByteArray.h"

/**
 * @brief 最近文件置换(LRU)缓存
 *
 * 以文件路径为键缓存文件内容(JZByteArray)，命中即视为“最近使用”并抬到队首；
 * 当累计代价(字节数, maxCost)或条目数(maxCount)超过上限时，自动淘汰最久未用(LRU)的条目。
 * 接口风格对齐 Qt 的 QCache(代价制 LRU)：setMaxCost / insert / object / take / remove / clear。
 *
 * 典型用途：反复读取的工作区文件缓存，避免每次搜索/读文件都重新打开磁盘。
 */
class JZFileWatch
{
public:
    /**
     * @brief 构造 LRU 文件缓存
     * @param maxCost   总代价上限(字节)，默认 20MB；<=0 表示不限制
     * @param maxCount  条目数上限，默认 0 表示不限制
     */
    explicit JZFileWatch(long long maxCost = 20LL * 1024 * 1024, int maxCount = 0);

    void setMaxCost(long long maxCost);
    long long maxCost() const;
    void setMaxCount(int maxCount);
    int maxCount() const;

    /** 插入/更新：以 path 为键存入内容；已存在则刷新内容并把该键抬到队首(最近使用)。 */
    void insert(const std::string& path, const JZByteArray& data);
    void insert(const std::string& path, const std::string& text);

    /** 取内容并把该键标记为最近使用；不存在返回空。 */
    JZByteArray get(const std::string& path) const;
    std::string getText(const std::string& path) const;

    bool contains(const std::string& path) const;

    /** 取出并移除该键的内容；不存在返回空。 */
    JZByteArray take(const std::string& path);

    /** 移除该键；返回是否真的存在并移除。 */
    bool remove(const std::string& path);

    bool empty() const;
    int count() const;
    long long totalCost() const;
    void clear();

    /** 手动触发淘汰：回收最久未用条目直至满足 maxCost 与 maxCount。 */
    void trim();

private:
    struct Entry
    {
        JZByteArray data;
        long long cost = 0;
    };
    using ListIter = std::list<std::string>::iterator;

    // 调用方需已持锁，且 key 必须存在于 m_map
    void touch(const std::string& key) const;
    // 调用方需已持锁
    void evictToLimit();

    JZ_DISABLE_COPY(JZFileWatch)

    mutable JZMutex m_mutex;
    mutable std::list<std::string> m_lru;   // 队首=最近使用, 队尾=最久未用
    std::unordered_map<std::string, std::pair<Entry, ListIter>> m_map;
    long long m_maxCost;
    int m_maxCount;        // <=0 表示不限制条目数
    long long m_totalCost;
};

#endif // JZFILEWATCH_H
