#include "JZMimeData.h"

#include <algorithm>
#include <cctype>
#include <cstring>

// url 里可能带扩展名大写 (.PNG) 或尾部空白, 统一成小写后比较
static std::string jzLower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = (char)std::tolower((unsigned char)s[i]);
    return s;
}

static bool jzIsImageFile(const std::string& path)
{
    const char* exts[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga" };
    std::string low = jzLower(path);
    for (const char* e : exts) {
        size_t n = std::strlen(e);
        if (low.size() >= n && low.compare(low.size() - n, n, e) == 0)
            return true;
    }
    return false;
}

const std::string& JZMimeData::textFormat()
{
    static std::string name = "text/plain";
    return name;
}

const std::string& JZMimeData::urlsFormat()
{
    static std::string name = "text/uri-list";
    return name;
}

const std::string& JZMimeData::imageFormat()
{
    static std::string name = "application/x-jz-image";
    return name;
}

JZMimeData::JZMimeData()
{
}

void JZMimeData::clear()
{
    m_data.clear();
    m_imageCache = JZImage();
    m_imageCached = false;
}

bool JZMimeData::isEmpty() const
{
    return m_data.empty() && !m_imageCached;
}

void JZMimeData::setData(const std::string& format, const JZByteArray& data)
{
    if (format.empty())
        return;
    m_data[format] = data;
    if (format == imageFormat()) {
        // 通用项里重新塞 image 数据 -> 之前缓存的那份图作废 (下次 image() 重新处理)
        m_imageCache = JZImage();
        m_imageCached = false;
    }
}

JZByteArray JZMimeData::data(const std::string& format) const
{
    auto it = m_data.find(format);
    if (it == m_data.end())
        return JZByteArray();
    return it->second;
}

bool JZMimeData::hasFormat(const std::string& format) const
{
    if (format == imageFormat())
        return hasImage();
    return m_data.find(format) != m_data.end();
}

std::vector<std::string> JZMimeData::formats() const
{
    std::vector<std::string> out;
    for (const auto& kv : m_data)
        out.push_back(kv.first);
    if (hasImage())
        out.push_back(imageFormat());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void JZMimeData::removeFormat(const std::string& format)
{
    m_data.erase(format);
    if (format == imageFormat()) {
        m_imageCache = JZImage();
        m_imageCached = false;
    }
}

bool JZMimeData::hasText() const
{
    return m_data.find(textFormat()) != m_data.end() ||
           m_data.find("text/plain;charset=utf-8") != m_data.end();
}

void JZMimeData::setText(const std::string& text)
{
    setData(textFormat(), JZByteArray(text.data(), (int)text.size()));
}

std::string JZMimeData::text() const
{
    JZByteArray buf = data(textFormat());
    if (buf.isEmpty())
        buf = data("text/plain;charset=utf-8");
    if (buf.isEmpty())
        return std::string();
    return std::string(buf.constData(), (size_t)buf.size());
}

bool JZMimeData::hasUrls() const
{
    auto it = m_data.find(urlsFormat());
    return it != m_data.end() && it->second.size() > 0;
}

void JZMimeData::setUrls(const std::vector<std::string>& paths)
{
    removeFormat(urlsFormat());
    if (paths.empty())
        return;

    // 每条一行 ('\n' 分隔, 与 uri-list 的行式约定同形)。空串不写。
    std::string joined;
    for (const auto& p : paths) {
        if (p.empty())
            continue;
        joined += p;
        joined += '\n';
    }
    if (!joined.empty())
        setData(urlsFormat(), JZByteArray(joined.data(), (int)joined.size()));
}

std::vector<std::string> JZMimeData::urls() const
{
    std::vector<std::string> out;
    if (!hasUrls())
        return out;

    JZByteArray buf = data(urlsFormat());
    const char* s = buf.constData();
    int len = buf.size();
    std::string cur;
    for (int i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\n' || c == '\r') {
            // uri-list 允许注释行 ('#'), 这里跳过
            if (!cur.empty() && cur[0] != '#')
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && cur[0] != '#')
        out.push_back(cur);
    return out;
}

bool JZMimeData::hasImage() const
{
    if (m_imageCached)
        return !m_imageCache.isNull();

    // 内联位图之外: 第一条 url 是可识别的图片文件也算 "有图" (见头文件说明)
    std::vector<std::string> paths = urls();
    return !paths.empty() && jzIsImageFile(paths[0]);
}

void JZMimeData::setImage(const JZImage& image)
{
    m_imageCache = image;
    m_imageCached = true;
    // 通用项里留一个空占位, 让 hasFormat(imageFormat()) / formats() 看得到它
    m_data[imageFormat()] = JZByteArray();
}

JZImage JZMimeData::image() const
{
    if (!m_imageCached)
        cacheImage();
    return m_imageCache;
}

void JZMimeData::cacheImage() const
{
    m_imageCached = true;   // 先置位: 无论成败都不再重复读盘
    m_imageCache = JZImage();

    std::vector<std::string> paths = urls();
    if (paths.empty() || !jzIsImageFile(paths[0]))
        return;
    JZImage decoded(paths[0]);
    if (!decoded.isNull())
        m_imageCache = decoded;
}
