#include "JZLineEdit.h"

#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZMouseEvent.h"
#include "JZKeyEvent.h"
#include "JZImeEvent.h"
#include "JZWindow.h"
#include "JZDisplayDevice.h"

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

// IME 窗口相对光标行的微调: 不同 IME / 字体若仍有偏差, 改这四个值
const int kImeCompOffsetX = 0;      // 组字窗锚点: 相对组字起点的水平偏移
const int kImeCompOffsetY = 0;      // 组字窗锚点: 相对字形顶部的垂直偏移
const int kImeCandOffsetX = 0;      // 候选窗左上角: 相对组字起点的水平偏移
const int kImeCandOffsetY = 4;      // 候选窗左上角: 相对字形底部的垂直偏移

} // namespace

JZ_OBJECT_IMPL(JZLineEdit, JZWidget)

JZLineEdit::JZLineEdit(JZWidget* parent)
    : JZWidget(parent)
    , sigTextChanged(this)
    , sigReturnPressed(this)
    , sigEditingFinished(this)
    , sigCursorPositionChanged(this)
{
    m_cursorTimer = new JZTimer(this);
    m_cursorTimer->setSingleShot(false);
    connect(&m_cursorTimer->sigTimeout, this, &JZLineEdit::onCursorTimer);
}

JZLineEdit::~JZLineEdit()
{
    setIMEContextEnabled(false);
}

JZSize JZLineEdit::sizeHint() const
{
    int w = m_width > 0 ? m_width : 160;
    int h = m_height > 0 ? m_height : 28;
    return JZSize(w, h);
}

// ==================== 属性 ====================

void JZLineEdit::setText(const std::string& text)
{
    JZString next = JZString::fromStdString(text);
    if (m_maxLength > 0 && next.size() > m_maxLength)
        next = next.left(m_maxLength);

    m_text = next;
    m_cursor = m_anchor = m_text.size();
    m_scrollX = 0;
    ensureCursorVisible();
    update();
    sigTextChanged.emit(text);
}

std::string JZLineEdit::text() const
{
    return m_text.toStdString();
}

void JZLineEdit::setPlaceholderText(const std::string& text)
{
    m_placeholder = JZString::fromStdString(text);
    update();
}

std::string JZLineEdit::placeholderText() const
{
    return m_placeholder.toStdString();
}

void JZLineEdit::setPassword(bool password)
{
    m_password = password;
    update();
}

bool JZLineEdit::isPassword() const
{
    return m_password;
}

void JZLineEdit::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    if (readOnly)
        setIMEContextEnabled(false);
    update();
}

bool JZLineEdit::isReadOnly() const
{
    return m_readOnly;
}

void JZLineEdit::setMaxLength(int n)
{
    m_maxLength = n;
    if (m_maxLength > 0 && m_text.size() > m_maxLength)
        m_text = m_text.left(m_maxLength);
}

int JZLineEdit::maxLength() const
{
    return m_maxLength;
}

void JZLineEdit::setCursorBlinkPeriod(int ms)
{
    m_blinkPeriod = ms;
    if (m_cursorTimer->isActive()) {
        m_cursorTimer->stop();
        if (ms > 0)
            m_cursorTimer->start(ms);
    }
}

void JZLineEdit::setTextMargins(int left, int right)
{
    m_marginLeft = left;
    m_marginRight = right;
    ensureCursorVisible();
    update();
}

// ==================== 光标与选区 ====================

int JZLineEdit::cursorPosition() const
{
    return m_cursor;
}

void JZLineEdit::setCursorPosition(int pos)
{
    int n = m_text.size();
    pos = std::max(0, std::min(pos, n));
    if (m_cursor == pos && m_anchor == pos)
        return;
    m_cursor = m_anchor = pos;
    ensureCursorVisible();
    resetCursorBlink();
    update();
    sigCursorPositionChanged.emit();
}

int JZLineEdit::selStart() const
{
    return std::min(m_cursor, m_anchor);
}

int JZLineEdit::selEnd() const
{
    return std::max(m_cursor, m_anchor);
}

bool JZLineEdit::hasSelection() const
{
    return m_cursor != m_anchor;
}

int JZLineEdit::selectionStart() const
{
    return selStart();
}

int JZLineEdit::selectionLength() const
{
    return selEnd() - selStart();
}

std::string JZLineEdit::selectedText() const
{
    if (!hasSelection())
        return std::string();
    return m_text.mid(selStart(), selectionLength()).toStdString();
}

void JZLineEdit::selectAll()
{
    m_anchor = 0;
    m_cursor = m_text.size();
    ensureCursorVisible();
    update();
}

void JZLineEdit::clearSelection()
{
    m_anchor = m_cursor;
    update();
}

void JZLineEdit::setSelection(int start, int length)
{
    int n = m_text.size();
    int s = std::max(0, std::min(start, n));
    int e = std::max(s, std::min(start + length, n));
    m_anchor = s;
    m_cursor = e;
    ensureCursorVisible();
    update();
}

void JZLineEdit::deleteSelection()
{
    if (!hasSelection())
        return;
    int s = selStart();
    m_text.remove(s, selectionLength());
    m_cursor = m_anchor = s;
}

// 上屏文字的唯一入口: 替换选区 -> 插入 -> 同步锚点
void JZLineEdit::insertText(const JZString& s)
{
    if (s.isEmpty())
        return;

    deleteSelection();

    JZString ins = s;
    if (m_maxLength > 0) {
        int room = m_maxLength - m_text.size();
        if (room <= 0)
            return;
        if (ins.size() > room)
            ins = ins.left(room);
    }

    m_text.insert(m_cursor, ins);
    m_cursor += ins.size();
    m_anchor = m_cursor;        // 必须同步, 否则刚输入的文字会被当成选区

    ensureCursorVisible();
    resetCursorBlink();
    update();
    sigTextChanged.emit(text());
}

void JZLineEdit::moveCursor(int pos, bool select)
{
    pos = std::max(0, std::min(pos, m_text.size()));
    if (select)
        m_cursor = pos;         // 锚点不动 -> 扩展选区
    else
        m_cursor = m_anchor = pos;

    ensureCursorVisible();
    resetCursorBlink();
    update();
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

void JZLineEdit::selectWordAt(int pos)
{
    int n = m_text.size();
    int s = std::max(0, std::min(pos, n));
    int e = s;
    while (s > 0 && isWordChar(m_text[s - 1].toUnicode())) --s;
    while (e < n && isWordChar(m_text[e].toUnicode()))     ++e;

    m_anchor = s;
    m_cursor = e;
    resetCursorBlink();
    update();
}

// ==================== 坐标计算 ====================

int JZLineEdit::textOriginX() const
{
    return m_marginLeft;
}

// 客户区 x -> 文本下标 (取最近的字符边界)
int JZLineEdit::indexFromX(int x) const
{
    JZFont font(kFontPixelSize);
    std::string utf8 = m_text.toStdString();
    int rel = x - textOriginX() + m_scrollX;

    int best = 0;
    int bestDist = INT_MAX;
    for (int i = 0; i <= m_text.size(); ++i) {
        int w = JZFontMetrics::textWidth(utf8, font, i);
        int d = (w > rel) ? (w - rel) : (rel - w);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// 光标相对文本起点的偏移 (不含滚动); 组字时光标落在组字串内
int JZLineEdit::cursorOffsetX() const
{
    JZFont font(kFontPixelSize);
    int x = JZFontMetrics::textWidth(m_text.toStdString(), font, m_cursor);
    if (m_composing && !m_comp.isEmpty()) {
        int n = std::max(0, std::min(m_compCursor, m_comp.size()));
        x += JZFontMetrics::textWidth(m_comp.toStdString(), font, n);
    }
    return x;
}

void JZLineEdit::ensureCursorVisible()
{
    int view = width() - m_marginLeft - m_marginRight;
    if (view <= 0)
        return;

    int cx = cursorOffsetX();
    if (cx - m_scrollX < 0)
        m_scrollX = std::max(0, cx - 8);
    else if (cx - m_scrollX > view)
        m_scrollX = cx - view + 8;
}

void JZLineEdit::resetCursorBlink()
{
    m_cursorOn = true;
    m_cursorTimer->stop();
    if (m_blinkPeriod > 0)
        m_cursorTimer->start(m_blinkPeriod);
}

void JZLineEdit::onCursorTimer()
{
    m_cursorOn = !m_cursorOn;
    update();
}

// ==================== 绘制 ====================

namespace {

// 显示文本: 密码模式把每个码点替换成 '*'
// 注意宽度测量同样要用它, 否则光标会与显示内容对不齐
JZString toDisplay(const JZString& s, bool password)
{
    if (!password)
        return s;

    JZString out;
    for (int i = 0; i < s.size(); ++i)
        out.push_back(JZChar('*'));
    return out;
}

} // namespace

void JZLineEdit::paintEvent(JZPainterEvent*)
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

    const int visH  = JZFontMetrics::visualHeight(font);
    const int yTop  = JZFontMetrics::centeredTextTop(font, h);              // 文字 cell 顶
    const int bTop  = yTop + JZFontMetrics::internalLeading(font);          // 字形可视顶

    const int clipW = w - m_marginLeft - m_marginRight;
    if (clipW <= 0)
        return;
    p.setClip(JZRect(m_marginLeft, 1, clipW, h - 2));

    const JZString disp   = toDisplay(m_text, m_password);
    const std::string u8  = disp.toStdString();
    const int x0 = textOriginX() - m_scrollX;

    if (m_text.isEmpty() && !m_composing) {
        // 空且无组字: 画占位提示
        if (!m_placeholder.isEmpty()) {
            p.setPen(JZPen(JZColor(0xA0, 0xA4, 0xAC), 1));
            p.drawText(x0, yTop, m_placeholder.toStdString().c_str());
        }
    } else if (hasSelection()) {
        // 选区: 深蓝底 + 白字
        const int ss  = selStart();
        const int se  = selEnd();
        const int xss = x0 + JZFontMetrics::textWidth(u8, font, ss);
        const int xse = x0 + JZFontMetrics::textWidth(u8, font, se);

        p.fillRect(JZRect(xss, bTop, xse - xss, visH), JZColor(0x33, 0x90, 0xFF));

        p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        if (ss > 0)
            p.drawText(x0, yTop, disp.left(ss).toStdString().c_str());
        p.setPen(JZPen(JZColor(0xFF, 0xFF, 0xFF), 1));
        p.drawText(xss, yTop, disp.mid(ss, se - ss).toStdString().c_str());
        p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        if (se < disp.size())
            p.drawText(xse, yTop, disp.mid(se).toStdString().c_str());
    } else {
        // 前缀 + 组字串 + 后缀
        const int wPre = JZFontMetrics::textWidth(u8, font, m_cursor);
        p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        if (m_cursor > 0)
            p.drawText(x0, yTop, disp.left(m_cursor).toStdString().c_str());

        int wComp = 0;
        if (m_composing && !m_comp.isEmpty()) {
            const JZString dispComp = toDisplay(m_comp, m_password);
            wComp = JZFontMetrics::textWidth(dispComp.toStdString(), font, dispComp.size());
            drawComposition(p, x0 + wPre, yTop, bTop, visH, dispComp);
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        }

        if (m_cursor < disp.size())
            p.drawText(x0 + wPre + wComp, yTop, disp.mid(m_cursor).toStdString().c_str());
    }

    // 光标
    if (focused && m_cursorOn && !m_readOnly) {
        const int cx = x0 + cursorOffsetX();
        p.fillRect(JZRect(cx, bTop, 2, visH), JZColor(0x10, 0x10, 0x10));
    }
}

// 组字串三遍绘制: 反显底色 -> 文字 -> 下划线。
// 顺序不能乱: 底色画在文字之后会盖掉字。
void JZLineEdit::drawComposition(JZPainter& p, int x, int textTop, int visTop, int visH,
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
            (i < static_cast<int>(m_compAttr.size())) ? m_compAttr[i] : JZImeAttr_input;

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
            (i < static_cast<int>(m_compAttr.size())) ? m_compAttr[i] : JZImeAttr_input;

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

namespace {

// 单调时钟毫秒数, 用于自建双击检测
long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void JZLineEdit::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() != JZMouseEvent::LeftButton)
        return;

    setFocus();

    // 点在本控件上而正在组字: 先把组字落屏, 再重新定位光标
    if (m_composing)
        completeComposition();

    const int x = event->localPos().x();
    const long long now = nowMs();

    // 自建双击检测: 不依赖消息层是否转发 WM_LBUTTONDBLCLK
    // (JZMouseEvent 目前没有 DoubleClick 类型)
    const bool isDoubleClick =
        (now - m_lastClickMs < 400) && (std::abs(x - m_lastClickX) <= 4);
    m_lastClickMs = now;
    m_lastClickX = x;

    if (isDoubleClick) {
        selectWordAt(indexFromX(x));
    } else {
        const int pos = indexFromX(x);
        m_cursor = m_anchor = pos;      // 普通点击: 清除选区
        m_dragging = true;
        resetCursorBlink();
        update();
    }

    updateIMEWindowPos();
}

void JZLineEdit::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_dragging)
        return;

    const int pos = indexFromX(event->localPos().x());
    if (pos == m_cursor)
        return;

    m_cursor = pos;                     // 锚点不动 -> 拖动出选区
    ensureCursorVisible();
    resetCursorBlink();
    update();
}

void JZLineEdit::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_dragging = false;
}

// ==================== 键盘 ====================

void JZLineEdit::keyPressEvent(JZKeyEvent* event)
{
    const uint32_t key = event->key();
    const bool shift = event->shift();
    const bool ctrl  = event->ctrl();

    // 组字期间按键全部交给 IME: 方向键/退格作用于组字串内部, 本控件不处理
    if (m_composing && key != VK_TAB) {
        event->ignore();
        return;
    }

    // ---- Ctrl 快捷键 ----
    if (ctrl) {
        switch (key) {
        case 'A': selectAll();                        event->accept(); return;
        case 'C': copyToClipboard();                  event->accept(); return;
        case 'X': if (!m_readOnly) cutToClipboard();  event->accept(); return;
        case 'V': if (!m_readOnly) pasteFromClipboard(); event->accept(); return;
        default: break;
        }
    }

    switch (key) {
    case VK_LEFT:
        if (shift)                 moveCursor(m_cursor - 1, true);
        else if (hasSelection())   moveCursor(selStart(), false);   // 先折叠到选区起点
        else                       moveCursor(m_cursor - 1, false);
        event->accept();
        return;

    case VK_RIGHT:
        if (shift)                 moveCursor(m_cursor + 1, true);
        else if (hasSelection())   moveCursor(selEnd(), false);     // 先折叠到选区终点
        else                       moveCursor(m_cursor + 1, false);
        event->accept();
        return;

    case VK_HOME:
        moveCursor(0, shift);
        event->accept();
        return;

    case VK_END:
        moveCursor(m_text.size(), shift);
        event->accept();
        return;

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
        event->accept();
        return;

    case VK_ESCAPE:
        if (hasSelection()) {
            clearSelection();
            event->accept();
            return;
        }
        break;      // 无选区时交回系统 / IME 处理

    case VK_RETURN:
        sigReturnPressed.emit();
        sigEditingFinished.emit();
        event->accept();
        return;

    case VK_TAB:
        break;      // 交回焦点系统做控件切换

    default:
        break;
    }

    // 可打印字符 (IME 上屏的内容不走这里, 走 imeEvent 的 ResultStr)
    if (!ctrl && !m_readOnly && !event->text().empty()) {
        insertText(JZString::fromStdString(event->text()));
        event->accept();
        return;
    }

    event->ignore();
}

void JZLineEdit::afterContentChanged()
{
    ensureCursorVisible();
    resetCursorBlink();
    update();
    updateIMEWindowPos();
    sigTextChanged.emit(text());
}

// ==================== 输入法 ====================

void JZLineEdit::imeEvent(JZImeEvent* event)
{
    if (m_readOnly)
        return;

    if (event->hasFlag(JZImeEvent::StartComposition)) {
        // 开始组字前先删掉选区, 否则新文字会插在选区里
        if (hasSelection())
            deleteSelection();
        m_composing = false;
        m_comp.clear();
        m_compAttr.clear();
        m_compCursor = 0;
    }

    if (event->hasFlag(JZImeEvent::CompStr)) {
        const std::string& compUtf8 = event->compString();
        m_comp = JZString::fromStdString(compUtf8);

        // 组字属性按 **UTF-16 下标** 给出, 换算成码点下标后重排
        // (非 BMP 字符在 UTF-16 里占两个单元, 直接按下标拷贝会错位)
        const std::vector<unsigned char>& attr = event->compAttr();
        m_compAttr.assign(static_cast<size_t>(m_comp.size()), JZImeAttr_input);
        for (size_t u16 = 0; u16 < attr.size(); ++u16) {
            int cpIndex = JZImeEvent::utf16ToCodePointIndex(compUtf8, static_cast<int>(u16));
            if (cpIndex >= 0 && cpIndex < static_cast<int>(m_compAttr.size()))
                m_compAttr[static_cast<size_t>(cpIndex)] = attr[u16];
        }

        m_composing = !m_comp.isEmpty();
    }

    if (event->hasFlag(JZImeEvent::CursorPos) || event->hasFlag(JZImeEvent::CompStr)) {
        // 光标位置同样是 UTF-16 下标
        m_compCursor = JZImeEvent::utf16ToCodePointIndex(event->compString(),
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

void JZLineEdit::updateIMEWindowPos()
{
    HWND hwnd = nativeHwndOf(this);
    if (!hwnd)
        return;

    HIMC hIMC = ImmGetContext(hwnd);
    if (!hIMC)
        return;

    JZWindow* win = window();
    const float scale = (win && win->device()) ? win->device()->scale() : 1.0f;
    JZFont font(kFontPixelSize);

    // 锚点取**组字起点**而非当前光标: 组字时光标会在组字串里移动,
    // 若用光标位置, 候选条左边缘会跟着一路右移, 看起来就是"偏了"
    const int startX = textOriginX() - m_scrollX
                     + JZFontMetrics::textWidth(m_text.toStdString(), font, selStart());
    const int visTop = JZFontMetrics::centeredTextTop(font, height())
                     + JZFontMetrics::internalLeading(font);
    const int visH   = JZFontMetrics::visualHeight(font);

    JZPoint gTop = mapToGlobal(JZPoint(startX, visTop));
    JZPoint gBot = mapToGlobal(JZPoint(startX, visTop + visH));

    POINT pt, ptBot;
    pt.x    = toPhysical(gTop.x(), scale);
    pt.y    = toPhysical(gTop.y(), scale);
    ptBot.x = toPhysical(gBot.x(), scale);
    ptBot.y = toPhysical(gBot.y(), scale);
    ClientToScreen(hwnd, &pt);
    ClientToScreen(hwnd, &ptBot);

    // 组字窗锚点: 微软拼音的候选条就是照这个点摆的
    COMPOSITIONFORM cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = pt.x + toPhysical(kImeCompOffsetX, scale);
    cf.ptCurrentPos.y = pt.y + toPhysical(kImeCompOffsetY, scale);
    ImmSetCompositionWindow(hIMC, &cf);

    // 候选窗左上角: 组字起点往下移一行
    // (CFS_CANDIDATEPOS 比 CFS_EXCLUDE 兼容性好, 后者部分 IME 支持不佳)
    CANDIDATEFORM cdf;
    ZeroMemory(&cdf, sizeof(cdf));
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = pt.x    + toPhysical(kImeCandOffsetX, scale);
    cdf.ptCurrentPos.y = ptBot.y + toPhysical(kImeCandOffsetY, scale);
    ImmSetCandidateWindow(hIMC, &cdf);

    ImmReleaseContext(hwnd, hIMC);
}

void JZLineEdit::cancelComposition()
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

void JZLineEdit::completeComposition()
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

void JZLineEdit::setIMEContextEnabled(bool enable)
{
    HWND hwnd = nativeHwndOf(this);
    if (!hwnd)
        return;

    if (enable) {
        if (!m_imeContext)
            return;
        ImmAssociateContext(hwnd, reinterpret_cast<HIMC>(m_imeContext));
    } else {
        cancelComposition();
        // 断开后 ImmGetContext 返回 NULL, 本窗口不再产生任何 IME 消息
        ImmAssociateContext(hwnd, nullptr);
    }
}

// ==================== 焦点 ====================

void JZLineEdit::focusInEvent(JZEvent*)
{
    // 注: IME 输入上下文 (IMC) 由所属窗口在 WM_SETFOCUS 时统一关联,
    // 所有控件共享同一个 HWND/IMC, 故控件焦点切换时不再重新关联/断开,
    // 以免断开影响同窗口内其它控件的输入。
    m_cursorOn = true;
    resetCursorBlink();
    update();
    updateIMEWindowPos();
}

void JZLineEdit::focusOutEvent(JZEvent*)
{
    cancelComposition();          // 放弃未上屏的组字
    m_dragging = false;
    m_cursorTimer->stop();
    m_cursorOn = false;
    update();
    sigEditingFinished.emit();
}

// ==================== 剪贴板 ====================

void JZLineEdit::copyToClipboard()
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

void JZLineEdit::cutToClipboard()
{
    if (m_readOnly || !hasSelection())
        return;

    copyToClipboard();
    deleteSelection();
    afterContentChanged();
}

void JZLineEdit::pasteFromClipboard()
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

            // 单行编辑: 换行统一替换成空格
            for (size_t i = 0; i < u8.size(); ++i) {
                if (u8[i] == '\r' || u8[i] == '\n')
                    u8[i] = ' ';
            }

            insertText(JZString::fromStdString(u8));
            return;
        }
    }
    CloseClipboard();
}
