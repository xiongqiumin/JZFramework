#include "JZTextImageMap.h"

#include <algorithm>
#include <cmath>

namespace {

// 显示尺寸上限默认值 (逻辑像素)。文本里图片是"一个字符", 但它的显示尺寸不能
// 无限大 —— 否则一张 4K 截图就能把行高撑成整屏, 并把几十 MB 像素长期留在内存。
// 320x240 约等于"聊天/表单里放得下的截图缩略图", 32bpp 时缓存 < 300KB。
const int kDefaultMaxDisplayW = 320;
const int kDefaultMaxDisplayH = 240;

int& maxDisplayWRef() { static int s = kDefaultMaxDisplayW; return s; }
int& maxDisplayHRef() { static int s = kDefaultMaxDisplayH; return s; }

// 把原图按**显示尺寸** (w,h) 缩放并预转成显示用位图 —— 入库只做一次,
// 之后每次绘制都是纯贴图 (无缩放、无格式转换、无缓存查找)。
// src 无像素 (只有尺寸的占位) 时两者都留空, 由调用方画占位框。
void makeDisplayImage(const JZImage& src, int w, int h,
                      JZImage& outImage, JZPixmap& outPixmap)
{
    outImage = JZImage();
    outPixmap = JZPixmap();

    if (src.isNull() || w <= 0 || h <= 0)
        return;

    JZImage disp = src;
    if (src.width() != w || src.height() != h) {
        // 缩小用双线性 (发糊明显), 放大用点采样 (保持原有观感)
        const bool shrink = (w < src.width() || h < src.height());
        const JZImage s = src.scaled(w, h,
            JZImage::AspectRatioMode::IgnoreAspectRatio,
            shrink ? JZImage::TransformationMode::SmoothTransformation
                   : JZImage::TransformationMode::FastTransformation);
        if (!s.isNull())
            disp = s;               // 缩放失败 (理论不会) 则退回原图尺寸
    }

    outImage = disp;
    outPixmap = JZPixmap::fromImage(disp);
}

} // namespace

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

    // 显示尺寸上限: 超限**等比缩小** (显式给的尺寸同样受约束)
    clampToMaxDisplaySize(w, h);

    // 入库的是**显示尺寸**的像素 + 预转好的显示用位图, 不是原图
    JZImage dispImage;
    JZPixmap dispPixmap;
    makeDisplayImage(image, w, h, dispImage, dispPixmap);

    // ---- 幂等: 同一 key 复用同一码点 ----
    std::map<std::string, char32_t>::iterator it = m_byKey.find(key);
    if (it != m_byKey.end()) {
        Entry& e = m_entries[it->second];
        e.image = dispImage;
        e.pixmap = dispPixmap;
        e.width = w;
        e.height = h;
        return it->second;
    }

    const char32_t cp = nextFreeCode();
    if (cp == 0)
        return 0;

    Entry e;
    e.key = key;
    e.image = dispImage;
    e.pixmap = dispPixmap;
    e.width = w;
    e.height = h;
    e.refCount = 0;

    m_entries[cp] = e;
    m_byKey[key] = cp;

    m_nextCandidate = cp + 1;
    return cp;
}

void JZTextImageMap::setMaxDisplaySize(int w, int h)
{
    maxDisplayWRef() = w;
    maxDisplayHRef() = h;
}

int JZTextImageMap::maxDisplayWidth()
{
    return maxDisplayWRef();
}

int JZTextImageMap::maxDisplayHeight()
{
    return maxDisplayHRef();
}

void JZTextImageMap::clampToMaxDisplaySize(int& w, int& h)
{
    if (w <= 0 || h <= 0)
        return;

    const int mw = maxDisplayWRef();
    const int mh = maxDisplayHRef();

    // 收缩系数取两个方向里更严格的那个 -> 保证等比 (不拉伸)
    double f = 1.0;
    if (mw > 0 && w > mw)
        f = std::min(f, static_cast<double>(mw) / w);
    if (mh > 0 && h > mh)
        f = std::min(f, static_cast<double>(mh) / h);
    if (f >= 1.0)
        return;

    w = std::max(1, static_cast<int>(std::lround(w * f)));
    h = std::max(1, static_cast<int>(std::lround(h * f)));
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
