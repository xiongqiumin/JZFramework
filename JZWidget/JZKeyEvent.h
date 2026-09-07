#ifndef JZ_KEY_EVENT_H_
#define JZ_KEY_EVENT_H_

#include <inttypes.h>
#include <string>
#include "JZEvent.h"

// 键盘事件。
//   key:       框架统一键码 (JZKey), 由消息层从平台虚拟键码映射填入。
//   modifiers: Shift/Ctrl/Alt 组合 (文本控件据此区分 移动光标 / 扩展选区)
//   text:      该按键产生的可打印文本 (UTF-8); 功能键为空。
//              IME 上屏的内容不走这里, 而是走 JZImeEvent::ResultStr。

// 框架统一键码 (值与 Win32 VK_* 一致, 解耦对 <windows.h> 的依赖)。
// 业务代码用 JZKey::Tab 等, 不再直接写 VK_TAB。
enum JZKey : uint32_t {
    Key_Backspace = 0x08,
    Key_Tab        = 0x09,
    Key_Return     = 0x0D,   // Enter / Return
    Key_Shift      = 0x10,
    Key_Control    = 0x11,
    Key_Alt        = 0x12,   // Win32 VK_MENU
    Key_Pause      = 0x13,
    Key_CapsLock   = 0x14,
    Key_Escape     = 0x1B,
    Key_Space      = 0x20,
    Key_PageUp     = 0x21,   // Win32 VK_PRIOR
    Key_PageDown   = 0x22,   // Win32 VK_NEXT
    Key_End        = 0x23,
    Key_Home       = 0x24,
    Key_Left       = 0x25,
    Key_Up         = 0x26,
    Key_Right      = 0x27,
    Key_Down       = 0x28,
    Key_Insert     = 0x2D,
    Key_Delete     = 0x2E,
    Key_F1         = 0x70,
    Key_F2         = 0x71,
    Key_F3         = 0x72,
    Key_F4         = 0x73,
    Key_F5         = 0x74,
    Key_F6         = 0x75,
    Key_F7         = 0x76,
    Key_F8         = 0x77,
    Key_F9         = 0x78,
    Key_F10        = 0x79,
    Key_F11        = 0x7A,
    Key_F12        = 0x7B,
};

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
