#include "JZFontMetrics.h"

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
        return s_engine->textWidth(text, font.size());
    return estimateWidth(text, font.size());
}

int JZFontMetrics::textHeight(const JZFont& font)
{
    if (s_engine)
        return s_engine->textHeight(font.size());
    return font.size() + 4;   // 估算行高 (字高 + 余量)
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
        unsigned char c = (unsigned char)text[i];
        uint32_t cp = c;
        int len = 1;
        if (c >= 0xF0)      { cp = c & 0x07u; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1Fu; len = 2; }

        bool valid = true;
        for (int k = 1; k < len; ++k) {
            if (i + k >= text.size()) { valid = false; break; }
            unsigned char cc = (unsigned char)text[i + k];
            if ((cc & 0xC0u) != 0x80u) { valid = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }

        w += (cp >= 0x2E80) ? pixelSize : (pixelSize * 11) / 20;
        i += valid ? len : 1;
    }
    return w;
}
