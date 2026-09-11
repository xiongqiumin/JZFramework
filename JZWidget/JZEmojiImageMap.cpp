#include "JZEmojiImageMap.h"

#include "JZResources.h"
#include "JZUtf8Iterator.h"

#include <algorithm>
#include <cstdio>
#include <vector>

// jz_add_resources 为 assets/emoji/emoji.qrc 生成的注册入口 (prefix="/emoji")。
// 资源**不会自动注册**, 必须显式调用; 这里在首次使用时懒调用一次,
// 于是使用方无需关心初始化顺序。
bool jzres_emoji_init();

namespace {

// 只注册一次 (jzres_emoji_init 内部也幂等, 这里多加一层避免重复函数调用)
void ensureEmojiResources()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;
    jzres_emoji_init();
}

// 码点 -> Twemoji 风格的文件名片段 (小写十六进制, 无前导零)
std::string codepointToKey(char32_t cp)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(cp));
    return std::string(buf);
}

// 码点序列 -> "1f468-200d-1f469" 形式的资源名
std::string joinKey(const std::vector<char32_t>& cps)
{
    std::string key;
    for (size_t i = 0; i < cps.size(); ++i) {
        if (i > 0)
            key.push_back('-');
        key += codepointToKey(cps[i]);
    }
    return key;
}

// 从内置资源读一张 PNG 并解码; 不存在或解码失败返回 isNull()
JZImage loadFromResources(const std::string& key)
{
    ensureEmojiResources();             // 首次调用时注册内置 emoji 资源

    // 资源名格式固定为 ":/<group>/<path>" (jz_resources.py 生成时带 ':' 前缀),
    // 本模块的资源组是 emoji (见 assets/emoji/emoji.qrc 的 prefix="/emoji")。
    // 先用 contains 判断: "图集里没有这个 emoji" 是**预期内**的情况
    // (会退化成字体绘制), 不该走 find 打 jzWarning 刷屏。
    const std::string name = std::string(":/emoji/") + key + ".png";
    if (!JZResources::instance()->contains(name))
        return JZImage();

    const JZResourceData* res = JZResources::instance()->find(name);
    if (!res || !res->data || res->size <= 0)
        return JZImage();

    return JZImage::decode(JZByteArray(reinterpret_cast<const char*>(res->data),
                                       res->size));
}

// UTF-8 文本 -> 码点序列
std::vector<char32_t> toCodepoints(const std::string& utf8)
{
    std::vector<char32_t> cps;
    JZUtf8Iterator it(utf8);
    while (it.isValid()) {
        cps.push_back(it.codepoint());
        ++it;
    }
    return cps;
}

} // namespace

JZEmojiImageMap& JZEmojiImageMap::instance()
{
    static JZEmojiImageMap s_map;
    return s_map;
}

std::string JZEmojiImageMap::resourceKey(const std::vector<char32_t>& cps)
{
    return joinKey(cps);
}

std::string JZEmojiImageMap::resourceKey(const std::string& emojiUtf8)
{
    return joinKey(toCodepoints(emojiUtf8));
}

const JZImage* JZEmojiImageMap::find(const std::string& emoji)
{
    return findByCodepoints(toCodepoints(emoji));
}

const JZImage* JZEmojiImageMap::findByCodepoints(const std::vector<char32_t>& cps)
{
    if (cps.empty())
        return nullptr;

    const std::string key = joinKey(cps);
    std::map<std::string, CacheEntry>::iterator it = m_cache.find(key);
    if (it != m_cache.end()) {
        if (!it->second.resolved)
            return nullptr;
        return it->second.image.isNull() ? nullptr : &it->second.image;
    }

    return load(cps);
}

const JZPixmap* JZEmojiImageMap::findPixmap(const std::string& emoji, int boxSize)
{
    return findPixmap(toCodepoints(emoji), boxSize);
}

const JZPixmap* JZEmojiImageMap::findPixmap(const std::vector<char32_t>& cps, int boxSize)
{
    // 原图先走一级缓存 (解码 + 完整的"完整序列 -> 首码点"退化逻辑都在那儿)
    const JZImage* src = findByCodepoints(cps);
    if (!src || src->isNull())
        return nullptr;

    int box = boxSize;
    if (box <= 0)
        box = std::max(src->width(), src->height());

    // 两级缓存键: 同一个 emoji 在不同显示尺寸下各存一份 (字号变化时各自可用)
    const std::string key = joinKey(cps) + "@" + std::to_string(box);
    std::map<std::string, JZPixmap>::iterator it = m_pixmaps.find(key);
    if (it != m_pixmaps.end())
        return it->second.isNull() ? nullptr : &it->second;

    // 按显示尺寸缩放 + 预转 (只做一次); 正方形显示尺寸与图集一致, 不拉伸
    const JZImage scaled =
        (src->width() == box && src->height() == box)
            ? *src
            : src->scaled(box, box,
                          JZImage::AspectRatioMode::IgnoreAspectRatio,
                          JZImage::TransformationMode::SmoothTransformation);

    m_pixmaps[key] = JZPixmap::fromImage(scaled);

    const JZPixmap& pm = m_pixmaps[key];
    return pm.isNull() ? nullptr : &pm;
}

const JZImage* JZEmojiImageMap::load(const std::vector<char32_t>& cps)
{
    // 1) 先按**完整码点序列**找 (正确处理 ZWJ 组合 / 肤色 / 旗帜)
    std::string key = joinKey(cps);
    JZImage img = loadFromResources(key);
    if (!img.isNull()) {
        CacheEntry e;
        e.image = img;
        e.resolved = true;
        m_cache[key] = e;
        return &m_cache[key].image;
    }

    // 2) 退化: 只取**首个码点** (组合序列缺图时至少画出基字符)
    if (cps.size() > 1) {
        std::vector<char32_t> first(1, cps[0]);
        const std::string key1 = joinKey(first);

        std::map<std::string, CacheEntry>::iterator it = m_cache.find(key1);
        if (it != m_cache.end()) {
            // 复用已解析结果, 并把完整序列也标记为同一个结果 (避免重复查找)
            CacheEntry e;
            e.image = it->second.image;
            e.resolved = true;
            m_cache[key] = e;
            if (!m_cache[key].image.isNull())
                return &m_cache[key].image;
            return nullptr;
        }

        JZImage img1 = loadFromResources(key1);
        CacheEntry e1;
        e1.image = img1;
        e1.resolved = true;
        m_cache[key1] = e1;

        CacheEntry e;
        e.image = img1;
        e.resolved = true;
        m_cache[key] = e;
        if (!m_cache[key].image.isNull())
            return &m_cache[key].image;
        return nullptr;
    }

    // 3) 单码点也没有: 记一条"确认没有", 避免每次都重新查表
    CacheEntry e;
    e.resolved = true;
    m_cache[key] = e;
    return nullptr;
}

void JZEmojiImageMap::clear()
{
    m_cache.clear();
    m_pixmaps.clear();
}
