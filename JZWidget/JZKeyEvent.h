#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include <inttypes.h>
#include <string>
#include "JZEvent.h"

// 键盘事件。
//   key:       平台虚拟键码 (Win32 VK_*, 与 Qt 的 Qt::Key 无关, 由消息层填入)
//   modifiers: Shift/Ctrl/Alt 组合 (文本控件据此区分 移动光标 / 扩展选区)
//   text:      该按键产生的可打印文本 (UTF-8); 功能键为空。
//              IME 上屏的内容不走这里, 而是走 JZImeEvent::ResultStr。
class JZKeyEvent : public JZEvent
{
public:
    enum Modifier {
        NoModifier    = 0x00,
        ShiftModifier = 0x01,
        CtrlModifier  = 0x02,
        AltModifier   = 0x04,
    };

    explicit JZKeyEvent(uint32_t key = 0, uint32_t modifiers = NoModifier,
                        const std::string& text = std::string(), bool repeat = false);

    uint32_t key() const;
    uint32_t modifiers() const;
    const std::string& text() const;

    // 长按产生的重复键 (由消息层根据 lParam 的 bit30 填入)
    bool isRepeat() const;

    // 按下 / 释放 (消息层对 WM_KEYUP 置 false)
    bool isPress() const;
    void setPress(bool press);

    bool shift() const { return (m_modifiers & ShiftModifier) != 0; }
    bool ctrl()  const { return (m_modifiers & CtrlModifier)  != 0; }
    bool alt()   const { return (m_modifiers & AltModifier)   != 0; }

private:
    uint32_t    m_key = 0;
    uint32_t    m_modifiers = NoModifier;
    std::string m_text;
    bool        m_repeat = false;
    bool        m_press = true;
};

#endif // JZ_KEY_EVENT_H_
