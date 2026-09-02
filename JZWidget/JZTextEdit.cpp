#include "JZTextEdit.h"

#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZMouseEvent.h"
#include "JZKeyEvent.h"
#include "JZImeEvent.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"
#include "JZResizeEvent.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

// windows.h 会定义 min/max 宏, 不加这个 std::min/std::max 会被破坏
#define NOMINMAX
#include <windows.h>
#include <imm.h>

// 虚拟键码: 非 Windows 平台补齐 (取值与 Win32 VK_* 一致)
#ifndef VK_BACK
#define VK_BACK   0x08
#define VK_TAB    0x09
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_HOME   0x24
#define VK_LEFT   0x25
#define VK_UP     0x26
#define VK_RIGHT  0x27
#define VK_DOWN   0x28
#define VK_END    0x23
#define VK_DELETE 0x2E
#endif

namespace {

const int kFontPixelSize = 14;      // 默认字号 (逻辑像素)
const int kLineGap       = 2;       // 行间距

// IME 窗口相对光标行的微调: 不同 IME / 字体若仍有偏差, 改这四个值
const int kImeCompOffsetX = 0;      // 组字窗锚点: 相对组字起点的水平偏移
const int kImeCompOffsetY = 0;      // 组字窗锚点: 相对字形顶部的垂直偏移
const int kImeCandOffsetX = 0;      // 候选窗左上角: 相对组字起点的水平偏移
const int kImeCandOffsetY = 4;      // 候选窗左上角: 相对字形底部的垂直偏移

// 单调时钟毫秒数, 用于自建双击检测
long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

JZ_OBJECT_IMPL(JZTextEdit, JZWidget)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZWidget(parent)
    , sigTextChanged(this)
    , sigCursorPositionChanged(this)
{
    m_cursorTimer = new JZTimer(this);
    m_cursorTimer->setSingleShot(false);
    connect(&m_cursorTimer->sigTimeout, this, &JZTextEdit::onCursorTimer);
    relayout();
}

JZTextEdit::~JZTextEdit()
{
    // 注: 输入上下文 (IMC) 是 HWND 级的, 由所属窗口统一关联/断开
    // (JZWindow::updateImeContext), 控件析构时不能去动它 —— 否则会把同窗口
    // 其它输入框的输入法一起关掉。
}

JZSize JZTextEdit::sizeHint() const
{
    int w = m_width > 0 ? m_width : 160;
    int h = m_height > 0 ? m_height : 70;
    return JZSize(w, h);
}

// ==================== 属性 ====================

void JZTextEdit::setText(const std::string& text)
{
    m_text = JZString::fromStdString(text);
    m_cursor = m_anchor = m_text.size();
    m_scrollX = m_scrollY = 0;
    m_desiredColumn = 0;
    relayout();
    ensureCursorVisible();
    update();
    sigTextChanged.emit(text);
}

std::string JZTextEdit::text() const
{
    return m_text.toStdString();
}

void JZTextEdit::setPlaceholderText(const std::string& text)
{
    m_placeholder = JZString::fromStdString(text);
    update();
}

std::string JZTextEdit::placeholderText() const
{
    return m_placeholder.toStdString();
}

void JZTextEdit::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    // 只读状态变化会影响 imeEnabled(), 让所属窗口重算 IMC 关联
    if (JZWindow* win = window())
        win->updateImeContext();
    update();
}

bool JZTextEdit::isReadOnly() const
{
    return m_readOnly;
}

bool JZTextEdit::imeEnabled() const
{
    return !m_readOnly;
}

void JZTextEdit::setLineWrap(bool wrap)
{
    m_lineWrap = wrap;
    relayout();
    update();
}

bool JZTextEdit::lineWrap() const
{
    return m_lineWrap;
}

void JZTextEdit::setCursorBlinkPeriod(int ms)
{
    m_blinkPeriod = ms;
    if (m_cursorTimer->isActive()) {
        m_cursorTimer->stop();
        if (ms > 0)
            m_cursorTimer->start(ms);
    }
}

void JZTextEdit::setTextMargins(int left, int top, int right, int bottom)
{
    m_marginLeft = left;
    m_marginTop = top;
    m_marginRight = right;
    m_marginBottom = bottom;
    relayout();
    ensureCursorVisible();
    update();
}

void JZTextEdit::resizeEvent(JZResizeEvent*)
{
    // 自动换行模式下宽度变化会改变行布局
    if (m_lineWrap)
        relayout();
    ensureCursorVisible();
}

// ==================== 行布局 ====================

int JZTextEdit::lineHeight() const
{
    JZFont font(kFontPixelSize);
    return JZFontMetrics::textHeight(font) + kLineGap;
}

// 按 '\n' 切分逻辑行。文本变化后必须重建。
void JZTextEdit::relayout()
{
    m_lines.clear();

    const int n = m_text.size();
    int idx = 0;
    int y = 0;

    while (idx <= n) {
        int eol = idx;
        while (eol < n && m_text[eol].toUnicode() != '\n')
            ++eol;

        LineLayout L;
        L.start = idx;
        L.length = eol - idx;
        L.y = y;
        m_lines.push_back(L);

        y += lineHeight();
        if (eol >= n)
            break;
        idx = eol + 1;      // 跳过 '\n'
    }
}

int JZTextEdit::lineOfIndex(int idx) const
{
    if (m_lines.empty())
        return 0;

    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
        const LineLayout& L = m_lines[static_cast<size_t>(i)];
        // 行尾 (idx == start + length) 仍属于本行; 换行符属于本行末尾
        if (idx >= L.start && idx <= L.start + L.length)
            return i;
    }
    return static_cast<int>(m_lines.size()) - 1;
}

void JZTextEdit::locateIndex(int idx, int& line, int& column) const
{
    line = lineOfIndex(idx);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    column = idx - L.start;
}

int JZTextEdit::indexFromLineColumn(int line, int column) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    return L.start + std::max(0, std::min(column, L.length));
}

int JZTextEdit::contentTop() const
{
    return m_marginTop - m_scrollY;
}

// 行内 x -> 列号 (取最近的字符边界)
int JZTextEdit::columnFromX(int line, int x) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    JZFont font(kFontPixelSize);
    const std::string u8 = m_text.mid(L.start, L.length).toStdString();
    const int rel = x - (m_marginLeft - m_scrollX);

    int best = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i <= L.length; ++i) {
        const int w = JZFontMetrics::textWidth(u8, font, i);
        const int d = (w > rel) ? (w - rel) : (rel - w);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

int JZTextEdit::indexFromXY(int x, int y) const
{
    if (m_lines.empty())
        return 0;

    const int top = contentTop();
    const int lh = lineHeight();

    int line = 0;
    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
        line = i;
        if (y < top + m_lines[static_cast<size_t>(i)].y + lh)
            break;
    }
    return indexFromLineColumn(line, columnFromX(line, x));
}

// ==================== 光标与选区 ====================

int JZTextEdit::cursorPosition() const
{
    return m_cursor;
}

int JZTextEdit::selStart() const
{
    return std::min(m_cursor, m_anchor);
}

int JZTextEdit::selEnd() const
{
    return std::max(m_cursor, m_anchor);
}

bool JZTextEdit::hasSelection() const
{
    return m_cursor != m_anchor;
}

int JZTextEdit::selectionStart() const
{
    return selStart();
}

int JZTextEdit::selectionLength() const
{
    return selEnd() - selStart();
}

std::string JZTextEdit::selectedText() const
{
    if (!hasSelection())
        return std::string();
    return m_text.mid(selStart(), selectionLength()).toStdString();
}

void JZTextEdit::setCursorPosition(int pos)
{
    const int n = m_text.size();
    pos = std::max(0, std::min(pos, n));
    if (m_cursor == pos && m_anchor == pos)
        return;

    m_cursor = m_anchor = pos;
    m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
    ensureCursorVisible();
    resetCursorBlink();
    update();
    sigCursorPositionChanged.emit();
}

void JZTextEdit::selectAll()
{
    m_anchor = 0;
    m_cursor = m_text.size();
    ensureCursorVisible();
    update();
}

void JZTextEdit::clearSelection()
{
    m_anchor = m_cursor;
    update();
}

void JZTextEdit::setSelection(int start, int length)
{
    const int n = m_text.size();
    const int s = std::max(0, std::min(start, n));
    const int e = std::max(s, std::min(start + length, n));
    m_anchor = s;
    m_cursor = e;
    ensureCursorVisible();
    update();
}

void JZTextEdit::deleteSelection()
{
    if (!hasSelection())
        return;
    const int s = selStart();
    m_text.remove(s, selectionLength());
    m_cursor = m_anchor = s;
    relayout();
}

// 上屏文字的唯一入口: 替换选区 -> 插入 -> 同步锚点 -> 重建行布局
void JZTextEdit::insertText(const JZString& s)
{
    if (s.isEmpty())
        return;

    deleteSelection();
    m_text.insert(m_cursor, s);
    m_cursor += s.size();
    m_anchor = m_cursor;        // 必须同步, 否则刚输入的内容会被当成选区

    relayout();
    ensureCursorVisible();
    resetCursorBlink();
    update();
    sigTextChanged.emit(text());
}

// 注意: 这里不更新 m_desiredColumn —— 上下移动时要沿用记忆列,
// 由水平移动的调用方负责刷新它。
void JZTextEdit::moveCursor(int pos, bool select)
{
    pos = std::max(0, std::min(pos, m_text.size()));
    if (select)
        m_cursor = pos;             // 锚点不动 -> 扩展选区
    else
        m_cursor = m_anchor = pos;

    ensureCursorVisible();
    resetCursorBlink();
    update();
}

// 上下移动: 用记忆列 m_desiredColumn 定位, 短行自动靠到行尾
void JZTextEdit::moveCursorVertical(int deltaLine, bool select)
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);

    const int target = std::max(0, std::min(line + deltaLine,
                                            static_cast<int>(m_lines.size()) - 1));
    moveCursor(indexFromLineColumn(target, m_desiredColumn), select);
}

// 双击选词: 字母数字下划线算词, 连续 CJK 汉字算一词
static bool isWordChar(char32_t c)
{
    if (c == '_')
        return true;
    if (c < 128)
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return (c >= 0x3400 && c <= 0x4DBF) ||   // CJK 扩展 A
           (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK 基本区
           (c >= 0xF900 && c <= 0xFAFF);     // CJK 兼容
}

void JZTextEdit::selectWordAt(int pos)
{
    const int n = m_text.size();
    int s = std::max(0, std::min(pos, n));
    int e = s;
    while (s > 0 && isWordChar(m_text[s - 1].toUnicode())) --s;
    while (e < n && isWordChar(m_text[e].toUnicode()))     ++e;

    m_anchor = s;
    m_cursor = e;
    resetCursorBlink();
    update();
}

void JZTextEdit::ensureCursorVisible()
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    // 垂直滚动
    const int viewH = height() - m_marginTop - m_marginBottom;
    if (viewH > 0) {
        if (L.y - m_scrollY < 0)
            m_scrollY = L.y;
        else if (L.y + lineHeight() - m_scrollY > viewH)
            m_scrollY = L.y + lineHeight() - viewH;
    }

    // 水平滚动 (组字时光标落在组字串内, 要算上组字前缀宽度)
    const int viewW = width() - m_marginLeft - m_marginRight;
    if (viewW > 0) {
        JZFont font(kFontPixelSize);
        int cx = JZFontMetrics::textWidth(
                     m_text.mid(L.start, L.length).toStdString(), font, column);
        if (m_composing && !m_comp.isEmpty()) {
            const int n = std::max(0, std::min(m_compCursor, m_comp.size()));
            cx += JZFontMetrics::textWidth(m_comp.toStdString(), font, n);
        }

        if (cx - m_scrollX < 0)
            m_scrollX = std::max(0, cx - 8);
        else if (cx - m_scrollX > viewW)
            m_scrollX = cx - viewW + 8;
    }
}

void JZTextEdit::resetCursorBlink()
{
    m_cursorOn = true;
    m_cursorTimer->stop();
    if (m_blinkPeriod > 0)
        m_cursorTimer->start(m_blinkPeriod);
}

void JZTextEdit::onCursorTimer()
{
    m_cursorOn = !m_cursorOn;
    update();
}

void JZTextEdit::afterContentChanged()
{
    relayout();
    ensureCursorVisible();
    resetCursorBlink();
    update();
    updateIMEWindowPos();
    sigTextChanged.emit(text());
}

// ==================== 绘制 ====================

void JZTextEdit::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    const int w = width();
    const int h = height();

    // 外框: 聚焦时高亮
    const bool focused = hasFocus();
    p.setBrush(JZBrush(JZColor(0xFF, 0xFF, 0xFF)));
    p.setPen(JZPen(focused ? JZColor(0x2A, 0x7F, 0xFF) : JZColor(0xC0, 0xC4, 0xCC),
                   focused ? 2 : 1));
    p.drawRoundRect(0, 0, w, h, 3);

    JZFont font(kFontPixelSize);
    p.setFont(font);

    const int lead  = JZFontMetrics::internalLeading(font);
    const int visH  = JZFontMetrics::visualHeight(font);   // textHeight - internalLeading
    const int lh    = lineHeight();
    const int top   = contentTop();

    const int clipW = w - m_marginLeft - m_marginRight;
    const int clipH = h - m_marginTop - m_marginBottom;
    if (clipW <= 0 || clipH <= 0)
        return;
    p.setClipLocal(JZRect(m_marginLeft, m_marginTop, clipW, clipH));

    // 行内垂直居中: 先按"可视高度"居中得到字形顶 bTop, 再减 internalLeading 得文字 cell 顶
    const int pad   = (lh - visH) / 2;

    // 空内容: 只画占位提示 + 光标
    if (m_text.isEmpty() && !m_composing) {
        if (!m_placeholder.isEmpty()) {
            p.setPen(JZPen(JZColor(0xA0, 0xA4, 0xAC), 1));
            p.drawText(m_marginLeft - m_scrollX, top + pad - lead,
                       m_placeholder.toStdString().c_str());
        }
        if (focused && m_cursorOn && !m_readOnly)
            p.fillRect(JZRect(m_marginLeft - m_scrollX, top + pad, 2, visH),
                       JZColor(0x10, 0x10, 0x10));
        return;
    }

    const bool selecting = hasSelection();
    const int ss = selStart();
    const int se = selEnd();

    int compLine = -1, compCol = 0;
    if (m_composing && !m_comp.isEmpty())
        locateIndex(m_cursor, compLine, compCol);

    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
        const LineLayout& L = m_lines[static_cast<size_t>(i)];
        const int yLine = top + L.y;
        const int bTop  = yLine + pad;              // 字形可视顶
        const int yText = yLine + pad - lead;       // 文字 cell 顶

        // 行不在可视区则跳过
        if (yLine + lh < m_marginTop || yLine > h - m_marginBottom)
            continue;

        const JZString lineText = m_text.mid(L.start, L.length);
        const std::string u8    = lineText.toStdString();
        const int x0 = m_marginLeft - m_scrollX;
        const int lineEnd = L.start + L.length;

        if (!selecting) {
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
            if (L.length > 0)
                p.drawText(x0, yText, u8.c_str());
        } else {
            // 本行与选区的交集
            const int ssIn = std::max(ss, L.start);
            const int seIn = std::min(se, lineEnd);

            if (seIn > ssIn) {
                const int xss = x0 + JZFontMetrics::textWidth(u8, font, ssIn - L.start);
                const int xse = x0 + JZFontMetrics::textWidth(u8, font, seIn - L.start);
                p.fillRect(JZRect(xss, bTop, xse - xss, visH), JZColor(0x33, 0x90, 0xFF));
            }

            // 三段: 选区前(黑) / 选区内(白) / 选区后(黑)
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
            if (ssIn > L.start)
                p.drawText(x0, yText, lineText.left(ssIn - L.start).toStdString().c_str());

            if (seIn > ssIn) {
                p.setPen(JZPen(JZColor(0xFF, 0xFF, 0xFF), 1));
                p.drawText(x0 + JZFontMetrics::textWidth(u8, font, ssIn - L.start), yText,
                           lineText.mid(ssIn - L.start, seIn - ssIn).toStdString().c_str());
                p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
            }

            if (seIn < lineEnd)
                p.drawText(x0 + JZFontMetrics::textWidth(u8, font, seIn - L.start), yText,
                           lineText.mid(seIn - L.start).toStdString().c_str());
        }

        // 组字串画在光标所在行
        if (i == compLine) {
            const int xComp = x0 + JZFontMetrics::textWidth(u8, font, compCol);
            drawComposition(p, xComp, yText, bTop, visH, m_comp);
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        }
    }

    // 光标
    if (focused && m_cursorOn && !m_readOnly) {
        int curLine = 0, curCol = 0;
        locateIndex(m_cursor, curLine, curCol);
        const LineLayout& L = m_lines[static_cast<size_t>(curLine)];

        int cx = m_marginLeft - m_scrollX
               + JZFontMetrics::textWidth(m_text.mid(L.start, L.length).toStdString(),
                                          font, curCol);
        if (m_composing && !m_comp.isEmpty()) {
            const int n = std::max(0, std::min(m_compCursor, m_comp.size()));
            cx += JZFontMetrics::textWidth(m_comp.toStdString(), font, n);
        }

        p.fillRect(JZRect(cx, top + L.y + pad, 2, visH), JZColor(0x10, 0x10, 0x10));
    }
}

// 组字串三遍绘制: 反显底色 -> 文字 -> 下划线。
// 顺序不能乱: 底色画在文字之后会盖掉字。
void JZTextEdit::drawComposition(JZPainter& p, int x, int textTop, int visTop, int visH,
                                 const JZString& dispComp)
{
    JZFont font(kFontPixelSize);
    const int n = dispComp.size();
    const int lineY = visTop + visH - 2;

    // 1) 反显底色
    int cx = x;
    for (int i = 0; i < n; ++i) {
        const int cw = JZFontMetrics::textWidth(dispComp.mid(i, 1).toStdString(), font);
        const unsigned char at =
            (i < static_cast<int>(m_compAttr.size())) ? m_compAttr[static_cast<size_t>(i)]
                                                      : JZImeAttr_input;

        if (at == JZImeAttr_targetConverted || at == JZImeAttr_targetNotConverted)
            p.fillRect(JZRect(cx, visTop, cw, visH), JZColor(0xD4, 0xE6, 0xFF));
        else if (at == JZImeAttr_converted || at == JZImeAttr_fixedConverted)
            p.fillRect(JZRect(cx, visTop, cw, visH), JZColor(0xEC, 0xF0, 0xF6));

        cx += cw;
    }

    // 2) 组字文字
    p.setPen(JZPen(JZColor(0x0B, 0x5C, 0xD6), 1));
    p.drawText(x, textTop, dispComp.toStdString().c_str());

    // 3) 下划线: 正在转换 粗实线 / 已转换 细实线 / 输入中 细灰线
    cx = x;
    for (int i = 0; i < n; ++i) {
        const int cw = JZFontMetrics::textWidth(dispComp.mid(i, 1).toStdString(), font);
        const unsigned char at =
            (i < static_cast<int>(m_compAttr.size())) ? m_compAttr[static_cast<size_t>(i)]
                                                      : JZImeAttr_input;

        JZColor col(0x90, 0x98, 0xA8);
        int thickness = 1;
        if (at == JZImeAttr_targetConverted || at == JZImeAttr_targetNotConverted) {
            col = JZColor(0x0B, 0x5C, 0xD6);
            thickness = 2;
        } else if (at == JZImeAttr_converted || at == JZImeAttr_fixedConverted) {
            col = JZColor(0x40, 0x50, 0x60);
        }

        p.setPen(JZPen(col, thickness));
        p.drawLine(cx, lineY, cx + cw, lineY);
        cx += cw;
    }
}

// ==================== 鼠标 ====================

void JZTextEdit::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton)
        return;

    setFocus();

    // 点在本控件上而正在组字: 先把组字落屏, 再重新定位光标
    if (m_composing)
        completeComposition();

    const int x = event->localPos().x();
    const int y = event->localPos().y();
    const long long now = nowMs();

    // 自建双击检测 (JZMouseEvent 目前没有 DoubleClick 类型)
    const bool isDoubleClick =
        (now - m_lastClickMs < 400) && (std::abs(x - m_lastClickX) <= 4);
    m_lastClickMs = now;
    m_lastClickX = x;

    if (isDoubleClick) {
        selectWordAt(indexFromXY(x, y));
    } else {
        m_cursor = m_anchor = indexFromXY(x, y);
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        m_dragging = true;
        resetCursorBlink();
        update();
    }

    updateIMEWindowPos();
}

void JZTextEdit::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_dragging)
        return;

    const int pos = indexFromXY(event->localPos().x(), event->localPos().y());
    if (pos == m_cursor)
        return;

    m_cursor = pos;                     // 锚点不动 -> 拖动出选区 (可跨行)
    m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
    ensureCursorVisible();
    resetCursorBlink();
    update();
}

void JZTextEdit::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_dragging = false;
}

// ==================== 键盘 ====================

void JZTextEdit::keyPressEvent(JZKeyEvent* event)
{
    const uint32_t key = event->key();
    const bool shift = event->shift();
    const bool ctrl  = event->ctrl();

    // 组字期间按键全部交给 IME: 方向键/退格作用于组字串内部
    if (m_composing && key != VK_TAB) {
        event->ignore();
        return;
    }

    if (m_lines.empty())
        relayout();

    // ---- Ctrl 快捷键 ----
    if (ctrl) {
        switch (key) {
        case 'A': selectAll();                           event->accept(); return;
        case 'C': copyToClipboard();                     event->accept(); return;
        case 'X': if (!m_readOnly) cutToClipboard();     event->accept(); return;
        case 'V': if (!m_readOnly) pasteFromClipboard(); event->accept(); return;
        default: break;     // Ctrl+Home / Ctrl+End 落到下面的 switch
        }
    }

    switch (key) {
    case VK_LEFT:
        if (shift)                 moveCursor(m_cursor - 1, true);
        else if (hasSelection())   moveCursor(selStart(), false);
        else                       moveCursor(m_cursor - 1, false);
        // 水平移动: 刷新记忆列
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;

    case VK_RIGHT:
        if (shift)                 moveCursor(m_cursor + 1, true);
        else if (hasSelection())   moveCursor(selEnd(), false);
        else                       moveCursor(m_cursor + 1, false);
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;

    case VK_UP:
        moveCursorVertical(-1, shift);
        event->accept();
        return;

    case VK_DOWN:
        moveCursorVertical(1, shift);
        event->accept();
        return;

    case VK_HOME: {
        int line = 0, col = 0;
        locateIndex(m_cursor, line, col);
        const int target = ctrl ? 0 : m_lines[static_cast<size_t>(line)].start;
        moveCursor(target, shift);
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;
    }

    case VK_END: {
        int line = 0, col = 0;
        locateIndex(m_cursor, line, col);
        const LineLayout& L = m_lines[static_cast<size_t>(line)];
        const int target = ctrl ? m_text.size() : L.start + L.length;
        moveCursor(target, shift);
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;
    }

    case VK_BACK:
        if (m_readOnly) break;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_cursor > 0) {
            m_text.remove(m_cursor - 1, 1);
            --m_cursor;
            m_anchor = m_cursor;
        }
        afterContentChanged();
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;

    case VK_DELETE:
        if (m_readOnly) break;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_cursor < m_text.size()) {
            m_text.remove(m_cursor, 1);
            m_anchor = m_cursor;
        }
        afterContentChanged();
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;

    case VK_RETURN:
        if (m_readOnly) break;
        insertText(JZString("\n"));     // 多行: 回车是换行, 不是"完成编辑"
        m_desiredColumn = 0;
        event->accept();
        return;

    case VK_ESCAPE:
        if (hasSelection()) {
            clearSelection();
            event->accept();
            return;
        }
        break;      // 无选区时交回系统 / IME

    case VK_TAB:
        break;      // 交回焦点系统做控件切换

    default:
        break;
    }

    // 可打印字符 (IME 上屏的内容不走这里, 走 imeEvent 的 ResultStr)
    if (!ctrl && !m_readOnly && !event->text().empty()) {
        insertText(JZString::fromStdString(event->text()));
        m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
        event->accept();
        return;
    }

    event->ignore();
}

// ==================== 输入法 ====================

void JZTextEdit::imeEvent(JZImeEvent* event)
{
    if (m_readOnly)
        return;

    if (event->hasFlag(JZImeEvent::StartComposition)) {
        // 开始组字前先删掉选区, 否则新文字会插在选区里
        if (hasSelection()) {
            deleteSelection();
            relayout();
        }
        m_composing = false;
        m_comp.clear();
        m_compAttr.clear();
        m_compCursor = 0;
    }

    if (event->hasFlag(JZImeEvent::CompStr)) {
        const std::string& compUtf8 = event->compString();
        m_comp = JZString::fromStdString(compUtf8);

        // 组字属性按 **UTF-16 下标** 给出, 换算成码点下标后重排
        const std::vector<unsigned char>& attr = event->compAttr();
        m_compAttr.assign(static_cast<size_t>(m_comp.size()), JZImeAttr_input);
        for (size_t u16 = 0; u16 < attr.size(); ++u16) {
            const int cpIndex = JZImeEvent::utf16ToCodePointIndex(compUtf8,
                                                                  static_cast<int>(u16));
            if (cpIndex >= 0 && cpIndex < static_cast<int>(m_compAttr.size()))
                m_compAttr[static_cast<size_t>(cpIndex)] = attr[u16];
        }

        m_composing = !m_comp.isEmpty();
    }

    if (event->hasFlag(JZImeEvent::CursorPos)) {
        // 换算基准必须是当前组字串 (m_comp): 单独 GCS_CURSORPOS 到来时
        // event->compString() 为空, 用它换算会把光标打到 0。
        m_compCursor = JZImeEvent::utf16ToCodePointIndex(m_comp.toStdString(),
                                                         event->compCursor());
        if (m_compCursor < 0)
            m_compCursor = 0;
        if (m_compCursor > m_comp.size())
            m_compCursor = m_comp.size();
    }

    if (event->hasFlag(JZImeEvent::ResultStr)) {
        const std::string& res = event->resultString();
        if (!res.empty())
            insertText(JZString::fromStdString(res));

        // 结果已上屏, 组字串立即清空 (不等 EndComposition)
        m_composing = false;
        m_comp.clear();
        m_compAttr.clear();
        m_compCursor = 0;
    }

    if (event->hasFlag(JZImeEvent::EndComposition)) {
        m_composing = false;
        m_comp.clear();
        m_compAttr.clear();
        m_compCursor = 0;
    }

    ensureCursorVisible();
    resetCursorBlink();
    update();

    // 组字内容变化或候选窗打开/变化, 都要把光标位置重新回写给 IME
    updateIMEWindowPos();
    event->accept();
}

// ==================== 平台相关: 光标位置回写给 IME ====================

namespace {

// 所属原生窗口句柄; 窗口尚未创建 (惰性) 时返回 nullptr
HWND nativeHwndOf(JZWidget* w)
{
    JZWindow* win = w->window();
    if (!win)
        return nullptr;
    return reinterpret_cast<HWND>(win->nativeHandle());
}

// 逻辑像素 -> 物理像素 (IME 的窗口坐标一律用物理像素)
LONG toPhysical(int v, float scale)
{
    return static_cast<LONG>(v * scale + 0.5f);
}

} // namespace

void JZTextEdit::updateIMEWindowPos()
{
    HWND hwnd = nativeHwndOf(this);
    if (!hwnd || m_lines.empty())
        return;

    HIMC hIMC = ImmGetContext(hwnd);
    if (!hIMC)
        return;

    JZWindow* win = window();
    const float scale = (win && win->device()) ? win->device()->scale() : 1.0f;
    JZFont font(kFontPixelSize);

    const int visH = JZFontMetrics::visualHeight(font);
    const int pad  = (lineHeight() - visH) / 2;

    // 锚点取**组字起点**: 组字时光标会在组字串里移动,
    // 若用光标位置, 候选条左边缘会跟着一路右移
    int line = 0, col = 0;
    locateIndex(selStart(), line, col);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    const int startX = m_marginLeft - m_scrollX
        + JZFontMetrics::textWidth(m_text.mid(L.start, L.length).toStdString(), font, col);
    const int startY = contentTop() + L.y + pad;

    // mapToScreen: 控件坐标 -> 屏幕逻辑坐标 (内部已做 ×scale / ClientToScreen /
    // ÷scale)。不要用 mapToGlobal —— 那是控件树内累加坐标, 只在根窗口位于
    // (0,0) 时才碰巧等于客户区坐标。
    const JZPoint sTop = mapToScreen(JZPoint(startX, startY));
    const JZPoint sBot = mapToScreen(JZPoint(startX, startY + visH));

    POINT pt, ptBot;
    pt.x    = toPhysical(sTop.x(), scale);
    pt.y    = toPhysical(sTop.y(), scale);
    ptBot.x = toPhysical(sBot.x(), scale);
    ptBot.y = toPhysical(sBot.y(), scale);

    // 组字窗锚点: 微软拼音的候选条就是照这个点摆的
    COMPOSITIONFORM cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = pt.x + toPhysical(kImeCompOffsetX, scale);
    cf.ptCurrentPos.y = pt.y + toPhysical(kImeCompOffsetY, scale);
    ImmSetCompositionWindow(hIMC, &cf);

    // 候选窗左上角: 组字起点往下移一行
    CANDIDATEFORM cdf;
    ZeroMemory(&cdf, sizeof(cdf));
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = pt.x    + toPhysical(kImeCandOffsetX, scale);
    cdf.ptCurrentPos.y = ptBot.y + toPhysical(kImeCandOffsetY, scale);
    ImmSetCandidateWindow(hIMC, &cdf);

    ImmReleaseContext(hwnd, hIMC);
}

void JZTextEdit::cancelComposition()
{
    HWND hwnd = nativeHwndOf(this);
    if (hwnd) {
        HIMC hIMC = ImmGetContext(hwnd);
        if (hIMC) {
            ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(hwnd, hIMC);
        }
    }
    m_composing = false;
    m_comp.clear();
    m_compAttr.clear();
    m_compCursor = 0;
}

void JZTextEdit::completeComposition()
{
    HWND hwnd = nativeHwndOf(this);
    if (hwnd) {
        HIMC hIMC = ImmGetContext(hwnd);
        if (hIMC) {
            ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(hwnd, hIMC);
        }
    }
    m_composing = false;
    m_comp.clear();
    m_compAttr.clear();
    m_compCursor = 0;
}

// ==================== 焦点 ====================

void JZTextEdit::focusInEvent(JZEvent*)
{
    // 注: IME 输入上下文 (IMC) 由所属窗口统一关联/断开 (JZWindow::updateImeContext,
    // 依据 imeEnabled())。这里只负责让输入法的字体/状态与本控件对齐。
    if (JZWindow* win = window())
        win->prepareIme(kFontPixelSize);

    m_cursorOn = true;
    resetCursorBlink();
    update();
    updateIMEWindowPos();
}

void JZTextEdit::focusOutEvent(JZEvent*)
{
    cancelComposition();          // 放弃未上屏的组字
    m_dragging = false;
    m_cursorTimer->stop();
    m_cursorOn = false;
    update();
}

// ==================== 剪贴板 ====================

void JZTextEdit::copyToClipboard()
{
    if (!hasSelection())
        return;

    const std::string u8 = selectedText();

    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* p = static_cast<wchar_t*>(GlobalLock(hMem));
        if (p) {
            MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), p, wlen);
            p[wlen] = 0;
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

void JZTextEdit::cutToClipboard()
{
    if (m_readOnly || !hasSelection())
        return;

    copyToClipboard();
    deleteSelection();
    afterContentChanged();
}

void JZTextEdit::pasteFromClipboard()
{
    if (m_readOnly)
        return;

    if (!OpenClipboard(nullptr))
        return;

    HANDLE hMem = GetClipboardData(CF_UNICODETEXT);
    if (hMem) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(hMem));
        if (p) {
            const int u8len = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
            std::string u8((size_t)(u8len > 1 ? u8len - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, p, -1, &u8[0], u8len, nullptr, nullptr);
            GlobalUnlock(hMem);
            CloseClipboard();

            // 多行: 保留换行, 只把 \r\n 归一成 \n
            std::string norm;
            norm.reserve(u8.size());
            for (size_t i = 0; i < u8.size(); ++i) {
                if (u8[i] == '\r') {
                    if (i + 1 < u8.size() && u8[i + 1] == '\n')
                        continue;
                    norm.push_back('\n');
                } else {
                    norm.push_back(u8[i]);
                }
            }

            insertText(JZString::fromStdString(norm));
            return;
        }
    }
    CloseClipboard();
}


