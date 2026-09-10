#ifndef JZ_EMOJI_H_
#define JZ_EMOJI_H_

#include <string>

// ==================== Emoji 码点语义 ====================
//
// 文本里的 emoji 与普通汉字/字母的**排版语义不同**:
//   - 它多为非 BMP 码点 (U+1F300..U+1FAFF 等), 占 4 字节 UTF-8;
//   - 主字体 (微软雅黑) 没有字形, 必须走**回退字体**才能画出来;
//   - 它在光标语义上应当被当作"一个整体"(与图片类似), 不该被拆成两个
//     代理对单元。
//
// 本类只做**纯码点判定**, 不持有任何资源, 也不依赖渲染后端:
// 段落切分 (JZAbstractTextEdit::enumerateSegments) 用它把 emoji 从文字里
// 单独切出来, 于是"测量 / 绘制 / 光标"三处可以按同一口径处理 emoji 回退。
//
// 注意: 这里刻意**不**做完整的 Unicode Emoji 属性表 (那需要几万行数据)。
// 覆盖的是实际会遇到的常见区间; 需要更全时只改 isEmoji() 的区间表即可。
class JZEmoji
{
public:
    // 是否为 emoji 码点 (含常见符号/图形/表情/交通/旗帜等区间)
    static bool isEmoji(char32_t cp);

    // 是否为"零宽连接符/变体选择符"等**依附于前一个 emoji 的修饰码点**。
    // 例如 U+FE0F (VS16) 跟在 U+2764 后面构成 ❤️。
    // 段落切分时它们应当并入前一个 emoji 段, 而不是单独成段。
    static bool isEmojiModifier(char32_t cp);

    // 该码点的 UTF-8 字节长度 (1..4); 非法码点返回 0
    static int utf8Length(char32_t cp);

    // 是否落在"普通字体通常有字形"的范围 (基本拉丁/CJK/常用标点)。
    // 供调用方快速判断"这段文字能否直接用主字体绘制"。
    static bool isCommonText(char32_t cp);
};

#endif // JZ_EMOJI_H_
