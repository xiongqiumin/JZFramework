#ifndef JZ_KEY_SEQUENCE_H_
#define JZ_KEY_SEQUENCE_H_

#include "JZKeyEvent.h"

#include <string>
#include <stdint.h>

// ---------------------------------------------------------------------------
// JZKeySequence —— 快捷键文本 <-> (键码, 修饰键) 的互转。对齐 Qt 的 QKeySequence。
//
//   JZKeySequence::parse("Ctrl+Shift+S", key, mods);     // -> 'S' + Ctrl|Shift
//   JZKeySequence::matches("Ctrl+A", ev);                // 按键事件是否命中
//   JZKeySequence::text('A', JZKeyEvent::CtrlModifier);  // -> "Ctrl+A"
//
// 文本口径(与 Qt 一致的地方 / 有意简化的地方):
//   · 名字**大小写不敏感**, 用 '+' 连接; 末段是主键, 前面的都是修饰键
//   · 修饰键认 Ctrl / Control / Shift / Alt(不认 Meta —— 引擎没有 Win 键语义)
//   · 主键认: 单字符字母数字(A-Z 0-9), F1-F12, 以及 Tab / Enter(Return) / Esc(Escape) /
//     Space / Backspace / Delete(Del) / Insert(Ins) / Home / End / PageUp(PgUp) /
//     PageDown(PgDown) / Left / Right / Up / Down / Pause / CapsLock
//   · ⚠ 有意不支持 Qt 的「多段序列」("Ctrl+K, Ctrl+C")与 "Ctrl++"(加号键) —— 引擎目前用不上,
//     真需要时在 parse 里加分支即可
//
// 谁会用它: JZAction::setShortcut() 的文本要在按键分发时匹配 —— 分发在
// JZWindow::dispatchShortcut(), 详见那里的说明。
// ---------------------------------------------------------------------------
class JZFRAMEWORK_API JZKeySequence
{
public:
    // 文本 -> (key, modifiers)。name 认不出的修饰键会被忽略; 主键认不出则返回 false
    // (key/modifiers 已被清零 —— 调用方不用先初始化)。
    static bool parse(const std::string& text, uint32_t& key, uint32_t& modifiers);

    // (key, modifiers) -> 规范化文本("Ctrl+A")。认不出的主键给 "0x41" 兜底写法(不返回空串,
    // 便于日志里看出到底配了什么)。
    static std::string text(uint32_t key, uint32_t modifiers);

    // 事件是否命中这条快捷键: 按下 + 主键相同 + 三个修饰键(Ctrl/Shift/Alt)**完全一致**。
    // (Win 键等其它修饰位不参与比较; event 为空/false 直接返回 false)
    static bool matches(const std::string& shortcutText, const JZKeyEvent* event);
};

#endif // JZ_KEY_SEQUENCE_H_
