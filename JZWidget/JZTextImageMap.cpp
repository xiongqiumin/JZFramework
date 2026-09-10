#include "JZTextImageMap.h"

JZTextImageMap& JZTextImageMap::instance()
{
    static JZTextImageMap s_map;
    return s_map;
}

// 线性找第一个未占用的码点 (图片数量级很小, 无需哈希)
char32_t JZTextImageMap::nextFreeCode() const
{
    char32_t cp = m_nextCandidate;
    if (cp < kPuaBase)
        cp = kPuaBase;

    while (cp <= kPuaEnd) {
        if (m_entries.find(cp) == m_entries.end())
            return cp;
        ++cp;
    }
    return 0;   // 私有区耗尽 (65534 张图, 实际不可能)
}

char32_t JZTextImageMap::add(const std::string& key, const JZImage& image,
                             int width, int height)
{
    if (key.empty())
        return 0;

    // width/height <= 0 时取图片原始尺寸
    int w = width;
    int h = height;
    if (w <= 0)
        w = image.width();
    if (h <= 0)
        h = image.height();

    if (image.isNull() && (w <= 0 || h <= 0))
        return 0;       // 既无像素又无尺寸: 无法布局

    // ---- 幂等: 同一 key 复用同一码点 ----
    std::map<std::string, char32_t>::iterator it = m_byKey.find(key);
    if (it != m_byKey.end()) {
        Entry& e = m_entries[it->second];
        e.image = image;
        e.width = w;
        e.height = h;
        return it->second;
    }

    const char32_t cp = nextFreeCode();
    if (cp == 0)
        return 0;

    Entry e;
    e.key = key;
    e.image = image;
    e.width = w;
    e.height = h;
    e.refCount = 0;

    m_entries[cp] = e;
    m_byKey[key] = cp;

    m_nextCandidate = cp + 1;
    return cp;
}

const JZTextImageMap::Entry* JZTextImageMap::find(char32_t cp) const
{
    std::map<char32_t, Entry>::const_iterator it = m_entries.find(cp);
    if (it == m_entries.end())
        return nullptr;
    return &it->second;
}

bool JZTextImageMap::sizeOf(char32_t cp, int& w, int& h) const
{
    const Entry* e = find(cp);
    if (!e) {
        w = 20;     // 兜底占位: 保证布局确定, 不会退化
        h = 20;
        return false;
    }
    w = e->width;
    h = e->height;
    return true;
}

void JZTextImageMap::retain(char32_t cp)
{
    std::map<char32_t, Entry>::iterator it = m_entries.find(cp);
    if (it != m_entries.end())
        ++it->second.refCount;
}

void JZTextImageMap::release(char32_t cp)
{
    std::map<char32_t, Entry>::iterator it = m_entries.find(cp);
    if (it != m_entries.end() && it->second.refCount > 0)
        --it->second.refCount;
}

int JZTextImageMap::refCount(char32_t cp) const
{
    const Entry* e = find(cp);
    return e ? e->refCount : 0;
}

void JZTextImageMap::collectUnused()
{
    for (std::map<char32_t, Entry>::iterator it = m_entries.begin();
         it != m_entries.end(); ) {
        if (it->second.refCount <= 0) {
            m_byKey.erase(it->second.key);
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void JZTextImageMap::clear()
{
    m_byKey.clear();
    m_entries.clear();
    m_nextCandidate = kPuaBase;
}
