#ifndef JZ_LINE_EDIT_H_
#define JZ_LINE_EDIT_H_

#include <vector>
#include "JZWidget.h"
#include "JZFont.h"
#include "JZString.h"
#include "JZSignal.h"
#include "JZTimer.h"

// ==================== 单行文本编辑框 (自绘) ====================
// 不使用原生 Edit 控件, 文本/光标/选区/组字串全部自绘。
//
// 索引约定: 内部一律按**码点**索引 (JZString); 而 IME 给的下标 (GCS_CURSORPOS、
// 组字属性数组) 是 UTF-16 单元, 两者在非 BMP 字符上会错位,
// 换算见 JZImeEvent 的静态工具函数。
//
// 与消息层的关系: 本类只提供事件入口 (keyPressEvent / imeEvent / focusXxxEvent),
// 由平台消息层把 WM_KEY* / WM_IME_* 翻译后投递进来。未接消息层时控件能正常显示,
// 只是无法输入。
class JZLineEdit : public JZWidget
{
    JZ_OBJECT_DECLARE(JZLineEdit)

public:
    explicit JZLineEdit(JZWidget* parent = nullptr);
    ~JZLineEdit();

    JZSize sizeHint() const override;

    // ---- 文本 (UTF-8 std::string) ----
    void setText(const std::string& text);
    std::string text() const;

    void setPlaceholderText(const std::string& text);
    std::string placeholderText() const;

    void setPassword(bool password);                // 显示为圆点/星号
    bool isPassword() const;

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

    void setMaxLength(int n);                       // 0 = 不限
    int  maxLength() const;

    // ---- AI 语义 ----
    std::string aiRole() const override { return "textbox"; }
    std::string aiText() const override { return m_text.toStdString(); }
    std::vector<JZAiAction> aiActions() const override { return {{"setText", "string 新文本"}}; }

    // ---- 光标与选区 ----
    int  cursorPosition() const;
    void setCursorPosition(int pos);
    bool hasSelection() const;
    void selectAll();
    void clearSelection();
    void setSelection(int start, int length);
    std::string selectedText() const;
    int  selectionStart() const;
    int  selectionLength() const;

    void setCursorBlinkPeriod(int ms);              // <=0 关闭闪烁
    void setTextMargins(int left, int right);

jz_signals:
    JZSignal<const std::string&> sigTextChanged;       // 文本变化 (含 IME 上屏)
    JZSigVoid sigReturnPressed;
    JZSigVoid sigEditingFinished;                   // 失焦 / 回车
    JZSigVoid sigCursorPositionChanged;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void imeEvent(JZImeEvent* event) override;
    void focusInEvent(JZEvent* event) override;
    void focusOutEvent(JZEvent* event) override;

private:
    int  selStart() const;
    int  selEnd() const;
    void deleteSelection();
    void insertText(const JZString& s);
    void moveCursor(int pos, bool select);
    void selectWordAt(int pos);
    int  indexFromX(int x) const;
    int  cursorOffsetX() const;      // 光标相对文本起点的偏移 (不含滚动)
    int  textOriginX() const;        // 文本绘制起点 x
    void ensureCursorVisible();
    void resetCursorBlink();
    void afterContentChanged();          // 增删内容后的统一收尾
    void copyToClipboard();
    void cutToClipboard();
    void pasteFromClipboard();
    void onCursorTimer();
    // 画组字串: 反显底色 -> 文字 -> 下划线 (三步顺序不能乱, 底色会盖字)
    void drawComposition(class JZPainter& p, int x, int textTop, int visTop, int visH,
                         const JZString& dispComp);

    // 平台相关: 实现放在 .cpp, 非 Win32 平台为空实现
    void updateIMEWindowPos();
    void cancelComposition();
    void completeComposition();          // 结束组字并把内容落屏
    void setIMEContextEnabled(bool enable);

private:
    JZString m_text;
    JZString m_placeholder;
    int  m_cursor = 0;               // 插入点, 也是选区的活动端
    int  m_anchor = 0;               // 选区锚点; == m_cursor 表示无选区
    int  m_scrollX = 0;              // 水平滚动 (逻辑像素)
    bool m_password = false;
    bool m_readOnly = false;
    int  m_maxLength = 0;
    int  m_marginLeft = 6;
    int  m_marginRight = 6;
    int  m_blinkPeriod = 500;
    bool m_cursorOn = false;
    bool m_dragging = false;
    JZTimer* m_cursorTimer = nullptr;

    long long m_lastClickMs = 0;         // 双击检测 (不依赖消息层)
    int  m_lastClickX = 0;

    // 组字 (composition) 状态
    bool m_composing = false;
    JZString m_comp;                        // 组字串 (未上屏)
    std::vector<unsigned char> m_compAttr;  // 每码点的属性 (JZImeAttr_*)
    int  m_compCursor = 0;                  // 光标在组字串中的码点位置

    void* m_imeContext = nullptr;           // 平台输入上下文 (Win32: HIMC)
};

#endif // JZ_LINE_EDIT_H_
