#include "JZFileWatch.h"

JZFileWatch::JZFileWatch(long long maxCost, int maxCount)
    : m_maxCost(maxCost)
    , m_maxCount(maxCount)
    , m_totalCost(0)
{
}

void JZFileWatch::setMaxCost(long long maxCost)
{
    {
        JZMutexLocker locker(&m_mutex);
        m_maxCost = maxCost;
        evictToLimit();
    }
}

long long JZFileWatch::maxCost() const
{
    JZMutexLocker locker(&m_mutex);
    return m_maxCost;
}

void JZFileWatch::setMaxCount(int maxCount)
{
    {
        JZMutexLocker locker(&m_mutex);
        m_maxCount = maxCount;
        evictToLimit();
    }
}

int JZFileWatch::maxCount() const
{
    JZMutexLocker locker(&m_mutex);
    return m_maxCount;
}

void JZFileWatch::touch(const std::string& key) const
{
    auto it = m_map.find(key);
    if (it != m_map.end())
        m_lru.splice(m_lru.begin(), m_lru, it->second.second);   // 抬到队首(最近使用)
}

void JZFileWatch::evictToLimit()
{
    while (!m_map.empty())
    {
        bool overCost  = (m_maxCost > 0 && m_totalCost > m_maxCost);
        bool overCount = (m_maxCount > 0 && (int)m_map.size() > m_maxCount);
        if (!overCost && !overCount)
            break;

        const std::string& backKey = m_lru.back();
        auto it = m_map.find(backKey);
        if (it == m_map.end())          // 防御：列表/表不一致时直接丢弃尾节点
        {
            m_lru.pop_back();
            continue;
        }
        m_totalCost -= it->second.first.cost;
        m_map.erase(it);
        m_lru.pop_back();
    }
}

void JZFileWatch::insert(const std::string& path, const JZByteArray& data)
{
    JZMutexLocker locker(&m_mutex);
    long long cost = (long long)data.size();

    auto it = m_map.find(path);
    if (it != m_map.end())
    {
        // 已存在：刷新内容，代价差额计入总计，并抬到队首
        m_totalCost -= it->second.first.cost;
        it->second.first.data = data;
        it->second.first.cost = cost;
        m_totalCost += cost;
        touch(path);
        evictToLimit();
        return;
    }

    Entry e;
    e.data = data;
    e.cost = cost;
    m_lru.push_front(path);
    m_map[path] = std::make_pair(e, m_lru.begin());
    m_totalCost += cost;
    evictToLimit();
}

void JZFileWatch::insert(const std::string& path, const std::string& text)
{
    JZByteArray buf;
    buf.append(text.data(), (int)text.size());
    insert(path, buf);
}

JZByteArray JZFileWatch::get(const std::string& path) const
{
    JZMutexLocker locker(&m_mutex);
    auto it = m_map.find(path);
    if (it == m_map.end())
        return JZByteArray();
    touch(path);
    return it->second.first.data;
}

std::string JZFileWatch::getText(const std::string& path) const
{
    JZByteArray b = get(path);
    if (b.size() == 0)
        return std::string();
    return std::string(b.data(), b.size());
}

bool JZFileWatch::contains(const std::string& path) const
{
    JZMutexLocker locker(&m_mutex);
    return m_map.find(path) != m_map.end();
}

JZByteArray JZFileWatch::take(const std::string& path)
{
    JZMutexLocker locker(&m_mutex);
    auto it = m_map.find(path);
    if (it == m_map.end())
        return JZByteArray();

    JZByteArray d = it->second.first.data;
    m_totalCost -= it->second.first.cost;
    m_lru.erase(it->second.second);
    m_map.erase(it);
    return d;
}

bool JZFileWatch::remove(const std::string& path)
{
    JZMutexLocker locker(&m_mutex);
    auto it = m_map.find(path);
    if (it == m_map.end())
        return false;

    m_totalCost -= it->second.first.cost;
    m_lru.erase(it->second.second);
    m_map.erase(it);
    return true;
}

bool JZFileWatch::empty() const
{
    JZMutexLocker locker(&m_mutex);
    return m_map.empty();
}

int JZFileWatch::count() const
{
    JZMutexLocker locker(&m_mutex);
    return (int)m_map.size();
}

long long JZFileWatch::totalCost() const
{
    JZMutexLocker locker(&m_mutex);
    return m_totalCost;
}

void JZFileWatch::clear()
{
    JZMutexLocker locker(&m_mutex);
    m_map.clear();
    m_lru.clear();
    m_totalCost = 0;
}

void JZFileWatch::trim()
{
    JZMutexLocker locker(&m_mutex);
    evictToLimit();
}
