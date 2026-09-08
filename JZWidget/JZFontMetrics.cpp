#include "JZFontMetrics.h"
#include "JZUtf8Iterator.h"

#include <cstdint>

namespace {

JZFontEngine* s_engine = nullptr;

} // namespace

void JZFontMetrics::setEngine(JZFontEngine* engine)
{
    s_engine = engine;
}

int JZFontMetrics::textWidth(const std::string& text, const JZFont& font)
{
    if (text.empty())
        return 0;
    if (s_engine)
        return s_engine->textWidth(text, font);
    return estimateWidth(text, font.size());
}

int JZFontMetrics::textHeight(const JZFont& font)
{
    if (s_engine)
        return s_engine->textHeight(font);
    return font.size() + 4;   // 估算行高 (字高 + 余量)
}

// 取 UTF-8 串前 charCount 个码点的字节前缀 (按码点计数, 不会切断多字节序列)
static std::string utf8Prefix(const std::string& text, int charCount)
{
    if (charCount <= 0)
        return std::string();

    int count = 0;
    size_t i = 0;
    while (i < text.size() && count < charCount) {
        char32_t cp;
        int len;
        if (!JZUtf8Iterator::decode(text.data(), text.size(), i, cp, len))
            break;                    // 非法/截断字节: 前缀止步于此
        if (i + len > text.size())
            break;                    // 截断的多字节序列留在前缀之外
        i += len;
        ++count;
    }
    return text.substr(0, i);
}

int JZFontMetrics::textWidth(const std::string& text, const JZFont& font, int charCount)
{
    if (text.empty() || charCount <= 0)
        return 0;

    std::string prefix = utf8Prefix(text, charCount);
    if (prefix.empty())
        return 0;

    if (s_engine)
        return s_engine->textWidth(prefix, font);
    return estimateWidth(prefix, font.size());
}

int JZFontMetrics::ascent(const JZFont& font)
{
    if (s_engine)
        return s_engine->ascent(font);
    return textHeight(font) * 4 / 5;
}

int JZFontMetrics::descent(const JZFont& font)
{
    if (s_engine)
        return s_engine->descent(font);
    return textHeight(font) - ascent(font);
}

int JZFontMetrics::internalLeading(const JZFont& font)
{
    if (s_engine)
        return s_engine->internalLeading(font);
    return 0;
}

int JZFontMetrics::centeredTextTop(const JZFont& font, int height)
{
    const int visH = textHeight(font) - internalLeading(font);
    return (height - visH) / 2 - internalLeading(font);
}

int JZFontMetrics::visualHeight(const JZFont& font)
{
    return textHeight(font) - internalLeading(font);
}

int JZFontMetrics::centerX(const std::string& text, const JZFont& font, int width)
{
    return (width - textWidth(text, font)) / 2;
}

int JZFontMetrics::centerY(const JZFont& font, int height)
{
    return (height - textHeight(font)) / 2;
}

// 无引擎时的兜底估算: 按 UTF-8 码点判宽, CJK (>= U+2E80) 记 1em, 其它记 0.55em
int JZFontMetrics::estimateWidth(const std::string& text, int pixelSize)
{
    int w = 0;
    size_t i = 0;
    while (i < text.size()) {
        char32_t cp;
        int len;
        if (JZUtf8Iterator::decode(text.data(), text.size(), i, cp, len)) {
            i += len;
        } else {
            cp = static_cast<unsigned char>(text[i]);   // 非法/截断字节: 按单字节窄字符跳过
            i += 1;
        }
        w += (cp >= 0x2E80) ? pixelSize : (pixelSize * 11) / 20;
    }
    return w;
}
