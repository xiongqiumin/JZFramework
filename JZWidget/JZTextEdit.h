#ifndef JZ_WIDGET_TEXTAREA_H_
#define JZ_WIDGET_TEXTAREA_H_

#include <vector>
#include "JZWidget.h"
#include "JZFont.h"
#include "JZString.h"
#include "JZSignal.h"
#include "JZTimer.h"

// ==================== 多行文本编辑框 (自绘) ====================
// 与 JZLineEdit **互相独立实现**: 多出一套行布局 (逻辑行切分、上下移动光标、
// 跨行选区、垂直滚动), 与单行版本的差异足够大, 合并会让两边都别扭。
//
// 索引约定 (码点 vs IME 的 UTF-16) 与 JZLineEdit 一致, 见 JZImeEvent 说明。
class JZTextEdit : public JZWidget
{
    JZ_OBJECT_DECLARE(JZTextEdit)

public:
    explicit JZTextEdit(JZWidget* parent = nullptr);
    ~JZTextEdit();

    JZSize sizeHint() const override;

    // ---- 文本 (UTF-8 std::string) ----
    void setText(const std::string& text);
    std::string text() const;

    void setPlaceholderText(const std::string& text);
    std::string placeholderText() const;

    void setReadOnly(bool readOnly);
    bool isReadOnly() const;

    // 自动换行; 关闭时超出宽度的内容靠水平滚动查看
    void setLineWrap(bool wrap);
    bool lineWrap() const;

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

    void setCursorBlinkPeriod(int ms);
    void setTextMargins(int left, int top, int right, int bottom);

    // ---- AI 语义 ----
    std::string aiRole() const override { return "textbox"; }
    std::string aiText() const override { return m_text.toStdString(); }
    std::vector<JZAiAction> aiActions() const override { return {{"setText", "string 新文本"}}; }

    // 可编辑时需要输入法; 窗口据此关联/断开 IMC
    bool imeEnabled() const override;

jz_signals:
    JZSignal<const std::string&> sigTextChanged;
    JZSigVoid sigCursorPositionChanged;

protected:
    void paintEvent(JZPainterEvent* event) override;
    void resizeEvent(JZResizeEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void imeEvent(JZImeEvent* event) override;
    void focusInEvent(JZEvent* event) override;
    void focusOutEvent(JZEvent* event) override;

private:
    // 一行的布局结果 (文本变化后由 relayout() 重建)
    struct LineLayout {
        int start = 0;    // 行首码点索引
        int length = 0;   // 本行字符数 (不含 '\n')
        int y = 0;        // 行顶部 y (逻辑像素, 未扣滚动)
    };

    void relayout();
    int  lineOfIndex(int idx) const;
    void locateIndex(int idx, int& line, int& column) const;
    int  indexFromLineColumn(int line, int column) const;
    int  columnFromX(int line, int x) const;
    int  indexFromXY(int x, int y) const;
    int  lineHeight() const;
    int  contentTop() const;          // 首行绘制 y (含滚动)
    int  selStart() const;
    int  selEnd() const;
    void deleteSelection();
    void insertText(const JZString& s);
    void moveCursor(int pos, bool select);
    void moveCursorVertical(int deltaLine, bool select);
    void selectWordAt(int pos);
    void ensureCursorVisible();
    void resetCursorBlink();
    void onCursorTimer();
    void afterContentChanged();
    void copyToClipboard();
    void cutToClipboard();
    void pasteFromClipboard();
    void drawComposition(class JZPainter& p, int x, int textTop, int visTop, int visH,
                         const JZString& dispComp);

    // 平台相关 (实现在 .cpp, 非 Win32 为空实现)
    void updateIMEWindowPos();
    void cancelComposition();
    void completeComposition();

private:
    JZString m_text;
    JZString m_placeholder;
    std::vector<LineLayout> m_lines;

    int  m_cursor = 0;                // 插入点, 也是选区的活动端
    int  m_anchor = 0;                // 选区锚点; == m_cursor 表示无选区
    int  m_scrollX = 0;               // 水平滚动 (逻辑像素)
    int  m_scrollY = 0;               // 垂直滚动 (逻辑像素)
    int  m_desiredColumn = 0;         // 上下移动光标时保持的目标列
    bool m_readOnly = false;
    bool m_lineWrap = false;
    int  m_marginLeft = 6;
    int  m_marginTop = 4;
    int  m_marginRight = 6;
    int  m_marginBottom = 4;
    int  m_blinkPeriod = 500;
    bool m_cursorOn = false;
    bool m_dragging = false;
    JZTimer* m_cursorTimer = nullptr;

    long long m_lastClickMs = 0;      // 双击检测 (不依赖消息层)
    int  m_lastClickX = 0;

    // 组字状态
    bool m_composing = false;
    JZString m_comp;
    std::vector<unsigned char> m_compAttr;
    int  m_compCursor = 0;
};

#endif
