#ifndef JZ_IME_EVENT_H_
#define JZ_IME_EVENT_H_
#include <string>
#include <vector>
#include "JZEvent.h"

// 挂在 JZEvent_user 之后, 不占用 JZCore 的预定义枚举
enum {
    JZEvent_ime      = JZEvent_user + 1,
    JZEvent_focusIn  = JZEvent_user + 2,
    JZEvent_focusOut = JZEvent_user + 3,
};

// ==================== 输入法(IME) 事件 ====================
// 由平台消息层 (Win32 的 WM_IME_*) 构造, 经 JZDisplayDevice 投递给当前焦点控件。
// 控件收到后自行绘制组字串, 并负责把光标位置回写给 IME (候选窗/组字窗定位)。
//
// 标志位取值与 Windows IMM 的 GCS_* 对齐, 消息层可直接按位映射:
//   WM_IME_STARTCOMPOSITION             -> StartComposition
//   WM_IME_COMPOSITION + GCS_COMPSTR    -> CompStr
//   WM_IME_COMPOSITION + GCS_RESULTSTR  -> ResultStr
//   WM_IME_COMPOSITION + GCS_CURSORPOS  -> CursorPos
//   WM_IME_ENDCOMPOSITION               -> EndComposition
//   WM_IME_NOTIFY + IMN_OPENCANDIDATE   -> Candidate
//
// 编码约定:
//   - compString / resultString 用 UTF-8 (框架统一编码);
//   - compCursor 与 compAttr 的下标是 **UTF-16 单元** (IMM 原生语义);
//   - 而控件内部文本按**码点**索引, 两者在非 BMP 字符 (emoji / 扩展区汉字,
//     即 UTF-16 代理对) 上会错位, 必须用下面的静态工具换算后再使用。
class JZFRAMEWORK_API JZImeEvent : public JZEvent
{
public:
    enum Flag {
        StartComposition = 0x0001,  // 开始组字
        EndComposition   = 0x0002,  // 结束组字 (组字窗关闭)
        CompStr          = 0x0008,  // 组字串内容变化
        CursorPos        = 0x0080,  // 光标在组字串内移动
        ResultStr        = 0x0800,  // 结果串上屏
        Candidate        = 0x0010,  // 候选窗打开/变化, 需要重新定位
    };

    explicit JZImeEvent(int flags = 0);

    int flags() const { return m_flags; }
    bool hasFlag(Flag f) const { return (m_flags & f) != 0; }
    void setFlag(Flag f) { m_flags |= f; }

    // 组字串 (未上屏, UTF-8)
    const std::string& compString() const { return m_comp; }
    void setCompString(const std::string& s) { m_comp = s; }

    // 结果串 (待上屏, UTF-8)
    const std::string& resultString() const { return m_result; }
    void setResultString(const std::string& s) { m_result = s; }

    // 组字串每字符属性 (ATTR_*); 下标按 UTF-16 单元计
    const std::vector<unsigned char>& compAttr() const { return m_attr; }
    void setCompAttr(const std::vector<unsigned char>& a) { m_attr = a; }

    // 光标在组字串中的位置 (UTF-16 下标)
    int compCursor() const { return m_compCursor; }
    void setCompCursor(int pos) { m_compCursor = pos; }

    // ---- UTF-16 下标 <-> 码点下标 换算 ----
    static int utf16Length(const std::string& utf8);
    static int utf16ToCodePointIndex(const std::string& utf8, int utf16Index);
    static int codePointToUtf16Index(const std::string& utf8, int cpIndex);

private:
    int m_flags = 0;
    std::string m_comp;
    std::string m_result;
    std::vector<unsigned char> m_attr;
    int m_compCursor = 0;
};

// 组字字符属性 (与 IMM 的 ATTR_* 一致), 便于控件决定下划线/反显样式
enum JZImeAttr {
    JZImeAttr_input            = 0x00,  // 输入中
    JZImeAttr_targetConverted  = 0x01,  // 正在转换的目标
    JZImeAttr_converted        = 0x02,  // 已转换
    JZImeAttr_targetNotConverted = 0x03,// 未转换的目标
    JZImeAttr_inputError       = 0x04,
    JZImeAttr_fixedConverted   = 0x05,
};

#endif // JZ_IME_EVENT_H_
