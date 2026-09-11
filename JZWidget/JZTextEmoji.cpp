#include "JZTextEmoji.h"
#include "JZFontMetrics.h"
#include "JZUtf8Iterator.h"
#include "JZEmoji.h"
#include "JZEmojiImageMap.h"

// emoji 段的显示宽度: 命中内置图集取位图宽(=box), 否则退回字体测宽(降级)。
int jzEmojiRunWidth(const std::string& seq, const JZFont& font)
{
    const int box = JZFontMetrics::textHeight(font);
    const JZPixmap* pm = JZEmojiImageMap::instance().findPixmap(seq, box);
    if (pm && !pm->isNull())
        return pm->width();
    return JZFontMetrics::measureRaw(seq, font);   // 降级: 仅字形估宽, 不走 emoji 分段(避免递归)
}

// 从字节位置 pos 起, 若起始码点是 emoji, 吸收后续修饰符 (VS16/ZWJ/肤色/标签),
// 返回完整 emoji 序列 (UTF-8) 与字节长度; 否则返回 false。
static bool nextEmojiSeq(const std::string& u8, size_t pos, size_t end,
                         std::string& outSeq, int& outLen)
{
    char32_t cp; int len;
    if (!JZUtf8Iterator::decode(u8.data(), u8.size(), pos, cp, len) || len <= 0)
        return false;
    if (!JZEmoji::isEmoji(cp))
        return false;

    std::string seq = u8.substr(pos, len);
    size_t j = pos + len;
    while (j < end) {
        char32_t nc; int nl;
        if (!JZUtf8Iterator::decode(u8.data(), u8.size(), j, nc, nl) || nl <= 0)
            break;
        if (JZEmoji::isEmojiModifier(nc) || JZEmoji::isEmoji(nc)) {
            seq += u8.substr(j, nl);
            j += nl;
            continue;
        }
        break;
    }
    outSeq = seq;
    outLen = static_cast<int>(j - pos);
    return true;
}

std::vector<JZTextEmojiRun> jzSegmentTextEmoji(const std::string& line, const JZFont& font)
{
    std::vector<JZTextEmojiRun> runs;
    const size_t n = line.size();
    size_t i = 0;
    while (i < n) {
        std::string seq; int seqLen;
        if (nextEmojiSeq(line, i, n, seq, seqLen)) {
            const int w = jzEmojiRunWidth(seq, font);
            JZTextEmojiRun r; r.emoji = true;  r.text = seq;
            r.byteStart = static_cast<int>(i); r.width = w;
            runs.push_back(r);
            i += seqLen;
            continue;
        }
        // 普通文本: 一路吃到下一个 emoji (或行尾)
        size_t j = i;
        char32_t c0; int l0;
        if (!JZUtf8Iterator::decode(line.data(), n, j, c0, l0) || l0 <= 0)
            j += 1;
        else
            j += l0;
        while (j < n) {
            char32_t nc; int nl;
            if (!JZUtf8Iterator::decode(line.data(), n, j, nc, nl) || nl <= 0)
                break;
            if (JZEmoji::isEmoji(nc))
                break;
            j += nl;
        }
        std::string txt = line.substr(i, j - i);
        const int w = JZFontMetrics::measureRaw(txt, font);   // 纯文本段, 无 emoji, 不会递归
        JZTextEmojiRun r; r.emoji = false; r.text = txt;
        r.byteStart = static_cast<int>(i); r.width = w;
        runs.push_back(r);
        i = j;
    }
    return runs;
}

bool jzContainsEmoji(const std::string& text)
{
    size_t i = 0, n = text.size();
    while (i < n) {
        char32_t cp; int len;
        if (!JZUtf8Iterator::decode(text.data(), n, i, cp, len) || len <= 0)
            return false;
        if (JZEmoji::isEmoji(cp))
            return true;
        i += len;
    }
    return false;
}

int jzSegmentTextEmojiWidth(const std::string& line, const JZFont& font)
{
    int total = 0;
    for (const auto& r : jzSegmentTextEmoji(line, font))
        total += r.width;
    return total;
}
