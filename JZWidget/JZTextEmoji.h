#ifndef JZ_TEXT_EMOJI_H_
#define JZ_TEXT_EMOJI_H_
#include <string>
#include <vector>
#include "JZFont.h"

// emoji 感知的一行文本分段结果: 与 JZPainter::drawText / JZAbstractTextEdit 同一口径。
// 文本里遇到 emoji 码点(含其后的 VS16/ZWJ/肤色等修饰符)整体切成一个不可分割的 emoji 段,
// 走 JZEmojiImageMap 取显示用位图; 其余连续普通文字合并成文本段。
struct JZFRAMEWORK_API JZTextEmojiRun {
    bool     emoji = false;       // true=emoji 段, false=普通文本段
    std::string text;             // 本段 UTF-8 (文本段或 emoji 序列)
    int      byteStart = 0;       // 段首在本行串中的字节下标
    int      width = 0;           // 显示宽度 (逻辑像素)
};

// 把一行文本切成文本段 / emoji 段, 并计算每段宽度。emoji 段命中内置图集时取位图宽,
// 否则退回字体测宽 (降级)。测量与绘制都走这里, 二者宽度一致。
// ⚠ 自由函数: 动态库模式下必须带 JZFRAMEWORK_API(使用方含 binary/code_agent 的 ChatView)。
JZFRAMEWORK_API std::vector<JZTextEmojiRun> jzSegmentTextEmoji(const std::string& line, const JZFont& font);

// 单个 emoji 序列的显示宽度: 命中内置图集取位图宽(=box), 否则退回字体测宽(降级)。
// 供换行等需要单独测量 emoji 宽度的场景复用(与 jzSegmentTextEmoji 同口径)。
JZFRAMEWORK_API int jzEmojiRunWidth(const std::string& seq, const JZFont& font);

// 整行 emoji 感知测宽 (段宽累加), 与 jzSegmentTextEmoji 同口径。供 JZFontMetrics::textWidth
// 复用, 使 "测量宽度 == 显示宽度" (否则 emoji 按回退字体字形量会把宽度算窄, 导致换行/光标错位)。
JZFRAMEWORK_API int jzSegmentTextEmojiWidth(const std::string& line, const JZFont& font);

// 快速判断字符串是否含 emoji, 供 textWidth 跳过纯文本的分段开销。
JZFRAMEWORK_API bool jzContainsEmoji(const std::string& text);

#endif // !JZ_TEXT_EMOJI_H_
