#ifndef JZ_ABSTRACT_TEXT_EDIT_H_
#define JZ_ABSTRACT_TEXT_EDIT_H_

#include <vector>
#include "JZWidget.h"
#include "JZFont.h"
#include "JZString.h"
#include "JZSignal.h"
#include "JZTimer.h"

// ==================== 文本编辑控件公共实现 ====================
// 单行 (JZLineEdit) 与多行 (JZTextEdit) 文本编辑的全部逻辑集中在本类:
// 文本/光标/选区、行布局、鼠标与键盘编辑、IME 组字、剪贴板、绘制、
// 以及平台 IME 窗口 (候选条) 定位。两个派生类只提供各自的语义入口
// (password/maxLength/lineWrap/默认尺寸) 与信号含义, 不再承载实现。
//
// 单行/多行的差异被 m_singleLine 收敛为少量分支, 以"行模型"统一:
//   - 行布局: 单行恒为一行 (文本入口把换行替换成空格, 保证不变量);
//   - 垂直:   仅多行参与垂直滚动/上下移动; 单行的"首行 y"取控件高度
//             内垂直居中, 与旧 JZLineEdit 的 centeredTextTop 逐像素一致;
//   - 回车:   单行发 sigReturnPressed/sigEditingFinished, 多行插入 '\n';
//   - 上/下键: 多行移动光标, 单行交回 (event->ignore(), 让父级滚动)。
//   - 密码:   仅单行有; 所有测宽/绘制/点击定位统一走 displayString 掩码。
//
// 索引约定: 内部一律按**码点**索引 (JZString); 而 IME 给的下标 (GCS_CURSORPOS、
// 组字属性数组) 是 UTF-16 单元, 两者在非 BMP 字符上会错位, 换算见 JZImeEvent。
//
// 与消息层的关系: 本类只提供事件入口 (keyPressEvent / imeEvent / focusXxxEvent),
// 由平台消息层把 WM_KEY* / WM_IME_* 翻译后投递进来。未接消息层时控件能正常
// 显示, 只是无法输入。
class JZAbstractTextEdit : public JZWidget
{
    JZ_OBJECT_DECLARE(JZAbstractTextEdit)

public:
    ~JZAbstractTextEdit() override;

    // ---- 文本 (UTF-8 std::string) ----
    void setText(const std::string& text);
    std::string text() const;

    void setPlaceholderText(const std::string& text);
    std::string placeholderText() const;

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

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
    void setTextMargins(int left, int top, int right, int bottom);  // 多行版本
    void setTextMargins(int left, int right);                       // 单行版本 (只改左右)

    // 可编辑 (非只读; 单行且非密码) 时需要输入法; 窗口据此关联/断开 IMC
    bool imeEnabled() const override;

    // ---- AI 语义 ----
    std::string aiRole() const override { return "textbox"; }
    std::string aiText() const override { return m_text.toStdString(); }
    std::vector<JZAiAction> aiActions() const override { return {{"setText", "string 新文本"}}; }

jz_signals:
    JZSignal<const std::string&> sigTextChanged;       // 文本变化 (含 IME 上屏)
    JZSigVoid sigCursorPositionChanged;
    JZSigVoid sigReturnPressed;                        // 仅单行模式: 回车
    JZSigVoid sigEditingFinished;                      // 仅单行模式: 失焦 / 回车

protected:
    explicit JZAbstractTextEdit(JZWidget* parent, bool singleLine);

    // ---- 子类语义配置 (派生类公共 API 转调; 具体实现全在本类) ----
    void setPasswordMode(bool password);               // JZLineEdit::setPassword
    bool isPasswordMode() const { return m_password; }
    void setMaxLengthLimit(int n);                     // JZLineEdit::setMaxLength (0 = 不限)
    int  maxLengthLimit() const { return m_maxLength; }
    void setLineWrapMode(bool wrap);                   // JZTextEdit::setLineWrap
    bool isLineWrapMode() const { return m_lineWrap; }

    // ---- 事件入口: 编辑/输入逻辑全在基类, 派生类无需覆写 ----
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void imeEvent(JZImeEvent* event) override;
    void focusInEvent(JZEvent* event) override;
    void focusOutEvent(JZEvent* event) override;

    bool m_singleLine = false;    // true: JZLineEdit; false: JZTextEdit
    bool m_password = false;      // 仅单行; 多行恒 false
    bool m_readOnly = false;
    bool m_lineWrap = false;      // 仅多行 (resize 重建行布局用)
    int  m_maxLength = 0;         // 0 = 不限 (仅单行会设置)
    int  m_marginLeft = 6;
    int  m_marginTop = 4;
    int  m_marginRight = 6;
    int  m_marginBottom = 4;

private:
    // 一行布局结果 (文本变化后由 relayout() 重建)
    struct LineLayout {
        int start = 0;    // 行首码点索引
        int length = 0;   // 本行字符数 (不含 '\n')
        int y = 0;        // 行顶部 y (逻辑像素, 未扣滚动; 单行恒 0)
    };

    void relayout();
    int  lineOfIndex(int idx) const;
    void locateIndex(int idx, int& line, int& column) const;
    int  indexFromLineColumn(int line, int column) const;
    int  columnFromX(int line, int x) const;
    int  indexFromXY(int x, int y) const;
    int  lineHeight() const;
    int  contentTop() const;                      // 首行文本区顶 y (含滚动 / 单行居中)
    JZString displayString(const JZString& s) const;  // 密码模式: 每码点变 '*'

    int  selStart() const;
    int  selEnd() const;
    void deleteSelection();
    void insertText(const JZString& s);
    void moveCursor(int pos, bool select);
    void moveCursorVertical(int deltaLine, bool select);
    void selectWordAt(int pos);
    void refreshDesiredColumn();
    void ensureCursorVisible();
    void resetCursorBlink();
    void onCursorTimer();
    void afterContentChanged();                   // 增删内容后的统一收尾
    void copyToClipboard();
    void cutToClipboard();
    void pasteFromClipboard();
    // 组字串绘制: 反显底色 -> 文字 -> 下划线 (顺序不能乱, 底色会盖字)
    void drawComposition(class JZPainter& p, int x, int textTop, int visTop, int visH,
                         const JZString& dispComp);

    // 平台相关: 实现在 .cpp, 非 Win32 平台为空实现
    void updateIMEWindowPos();
    void cancelComposition();
    void completeComposition();                   // 结束组字并把内容落屏

    JZString m_text;
    JZString m_placeholder;
    std::vector<LineLayout> m_lines;

    int  m_cursor = 0;                            // 插入点, 也是选区的活动端
    int  m_anchor = 0;                            // 选区锚点; == m_cursor 表示无选区
    int  m_scrollX = 0;                           // 水平滚动 (逻辑像素)
    int  m_scrollY = 0;                           // 垂直滚动 (仅多行)
    int  m_desiredColumn = 0;                     // 上下移动光标时保持的目标列
    int  m_blinkPeriod = 500;
    bool m_cursorOn = false;
    bool m_dragging = false;
    JZTimer* m_cursorTimer = nullptr;

    long long m_lastClickMs = 0;                  // 双击检测 (不依赖消息层)
    int  m_lastClickX = 0;

    // 组字 (composition) 状态
    bool m_composing = false;
    JZString m_comp;                              // 组字串 (未上屏)
    std::vector<unsigned char> m_compAttr;        // 每码点属性 (JZImeAttr_*)
    int  m_compCursor = 0;                        // 光标在组字串中的码点位置
};

#endif // JZ_ABSTRACT_TEXT_EDIT_H_
