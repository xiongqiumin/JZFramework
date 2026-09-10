#include "JZEmoji.h"

namespace {

// 常见 emoji 码点区间 (按 Unicode 15 的主要区块; 不含完整属性表)。
// 每个区间按 [lo, hi] 闭区间比较, 命中即认定为 emoji。
struct Range { char32_t lo; char32_t hi; };

const Range kEmojiRanges[] = {
    { 0x203C,  0x203C },   // ‼
    { 0x2049,  0x2049 },   // ⁉
    { 0x2194,  0x21AA },   // ↔..↪ (箭头符号里的 emoji 部分)
    { 0x231A,  0x231B },   // ⌚ ⌛
    { 0x23E9,  0x23FA },   // ⏩..⏺
    { 0x24C2,  0x24C2 },   // Ⓜ
    { 0x25AA,  0x25FE },   // ▪..◾ 等几何图形
    { 0x2600,  0x27BF },   // ☀..➿ (杂项符号 + 装饰符号, 大量 emoji)
    { 0x2934,  0x2935 },   // ⤴ ⤵
    { 0x2B05,  0x2B55 },   // ⬅..⭕
    { 0x3030,  0x3030 },   // 〰
    { 0x303D,  0x303D },   // 〽
    { 0x3297,  0x3299 },   // ㊗ ㊙
    { 0x1F000, 0x1F0FF },  // 麻将/扑克/多米诺
    { 0x1F100, 0x1F1FF },  // 带圈字母/区域指示符(国旗组合)
    { 0x1F200, 0x1F2FF },  // 带圈表意文字补充
    { 0x1F300, 0x1F5FF },  // 杂项符号与象形文字 (天气/建筑/动物)
    { 0x1F600, 0x1F64F },  // 表情符号
    { 0x1F650, 0x1F67F },  // 装饰性符号
    { 0x1F680, 0x1F6FF },  // 交通与地图符号
    { 0x1F700, 0x1F77F },  // 炼金术符号
    { 0x1F780, 0x1F7FF },  // 几何扩展图形
    { 0x1F800, 0x1F8FF },  // 补充箭头
    { 0x1F900, 0x1F9FF },  // 补充符号与象形文字 (手势/面部/服装等)
    { 0x1FA00, 0x1FAFF },  // 符号与象形文字扩展 A
    { 0x1FB00, 0x1FBFF },  // 传统计算符号
};

// 依附于前一个 emoji 的修饰码点
bool isModifierImpl(char32_t cp)
{
    return cp == 0xFE0E ||          // VS15 (文字呈现)
           cp == 0xFE0F ||          // VS16 (emoji 呈现)
           cp == 0x200D ||          // ZWJ (零宽连接符, 组合 emoji 序列)
           (cp >= 0x1F3FB && cp <= 0x1F3FF) ||   // 肤色修饰符
           (cp >= 0xE0020 && cp <= 0xE007F);     // 标签字符 (旗帜序列)
}

} // namespace

bool JZEmoji::isEmoji(char32_t cp)
{
    if (cp < 0x203C)
        return false;

    for (size_t i = 0; i < sizeof(kEmojiRanges) / sizeof(kEmojiRanges[0]); ++i) {
        if (cp >= kEmojiRanges[i].lo && cp <= kEmojiRanges[i].hi)
            return true;
    }
    return false;
}

bool JZEmoji::isEmojiModifier(char32_t cp)
{
    return isModifierImpl(cp);
}

int JZEmoji::utf8Length(char32_t cp)
{
    if (cp <= 0x7F)        return 1;
    if (cp <= 0x7FF)       return 2;
    if (cp <= 0xFFFF)      return 3;
    if (cp <= 0x10FFFF)    return 4;
    return 0;               // 超出 Unicode 范围
}

bool JZEmoji::isCommonText(char32_t cp)
{
    // 基本拉丁 / 拉丁补充 / 常用标点 / 中日韩 / 全角形式
    return cp < 0x2000 ||                          // 含 ASCII 与拉丁扩展
           (cp >= 0x2000 && cp <= 0x206F) ||       // 常用标点 (含少量非 emoji)
           (cp >= 0x3000 && cp <= 0x303F) ||       // CJK 符号标点
           (cp >= 0x3040 && cp <= 0x30FF) ||       // 假名
           (cp >= 0x3400 && cp <= 0x4DBF) ||       // CJK 扩展 A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||       // CJK 基本区
           (cp >= 0xAC00 && cp <= 0xD7AF) ||       // 谚文
           (cp >= 0xF900 && cp <= 0xFAFF) ||       // CJK 兼容
           (cp >= 0xFF00 && cp <= 0xFFEF);         // 全角/半角形式
}
