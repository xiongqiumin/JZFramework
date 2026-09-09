#include "JZAbstractTextEdit.h"

#include "JZUndoStack.h"

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

namespace {

const int kFontPixelSize = 14;      // 默认字号 (逻辑像素)
const int kLineGap       = 2;       // 行间距 (多行)

// IME 窗口相对光标行的微调: 不同 IME / 字体若仍有偏差, 改这四个值
const int kImeCompOffsetX = 0;      // 组字窗锚点: 相对组字起点的水平偏移
const int kImeCompOffsetY = 0;      // 组字窗锚点: 相对字形顶部的垂直偏移
const int kImeCandOffsetX = 0;      // 候选窗左上角: 相对组字起点的水平偏移
const int kImeCandOffsetY = 4;      // 候选窗左上角: 相对字形底部的垂直偏移

// 单行模式文本入口的换行清洗: 换行统一替换成空格 (单行不允许出现换行)
JZString flattenNewlines(const JZString& s)
{
    bool hasNewline = false;
    for (int i = 0; i < s.size(); ++i) {
        const char32_t c = s[i].toUnicode();
        if (c == '\n' || c == '\r') {
            hasNewline = true;
            break;
        }
    }
    if (!hasNewline)
        return s;

    JZString out;
    for (int i = 0; i < s.size(); ++i) {
        const char32_t c = s[i].toUnicode();
        out.push_back((c == '\n' || c == '\r') ? JZChar(' ') : s[i]);
    }
    return out;
}

// 双击选词: 字母数字下划线算词, 连续 CJK 汉字算一词
bool isWordChar(char32_t c)
{
    if (c == '_')
        return true;
    if (c < 128)
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return (c >= 0x3400 && c <= 0x4DBF) ||   // CJK 扩展 A
           (c >= 0x4E00 && c <= 0x9FFF) ||   // CJK 基本区
           (c >= 0xF900 && c <= 0xFAFF);     // CJK 兼容
}

// 单调时钟毫秒数, 用于自建双击检测 / 撤销合并时间窗
long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 连续编辑合并时间窗: 5 秒内的相邻编辑 (输入/删除/IME 上屏) 记为一撤销步,
// 超过 5 秒另起一步 (2026-09-03 约定)。
const long long kUndoMergeWindowMs = 5000;

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

// ==================== 文本编辑撤销命令 ====================
// 一步命令 = 一串"补丁", 每个补丁是 [pos, pos+len) 的文本替换:
//   redo  = 正序应用所有补丁 (把 removed 换成 inserted);
//   undo  = 逆序反向应用 (把 inserted 换回 removed)。
// 连续编辑通过 JZUndoStack 的合并协议合并: 新命令与栈顶命令时间差在
// kUndoMergeWindowMs (5 秒) 内且都不是"独立命令"(粘贴) 时, 栈顶命令
// 把新命令的补丁追加进来, 于是撤销一步回到整段编辑之前。
class JZAbstractTextEdit::EditCommand : public JZUndoCommand
{
public:
    struct Patch {
        int pos;
        JZString removed;
        JZString inserted;
    };

    EditCommand(JZAbstractTextEdit* owner,
                int pos, const JZString& removed, const JZString& inserted,
                bool isolated, long long timeMs,
                int cursorBefore, int anchorBefore,
                int cursorAfter, int anchorAfter)
        : m_owner(owner)
        , m_isolated(isolated)
        , m_timeMs(timeMs)
        , m_cursorBefore(cursorBefore)
        , m_anchorBefore(anchorBefore)
        , m_cursorAfter(cursorAfter)
        , m_anchorAfter(anchorAfter)
    {
        m_patches.push_back(Patch{ pos, removed, inserted });
    }

    int id() const override { return 1; }   // 所有文本编辑命令同 id -> 可尝试合并

    bool canMergeWith(const JZUndoCommand* other) const override
    {
        if (m_isolated)
            return false;
        const EditCommand* o = dynamic_cast<const EditCommand*>(other);
        if (!o || o->m_isolated || o->m_owner != m_owner)
            return false;
        // 时间窗: 新命令发生时刻与栈顶命令最后编辑时刻相隔 <= 5 秒
        const long long dt = o->m_timeMs - m_timeMs;
        return dt >= 0 && dt <= kUndoMergeWindowMs;
    }

    void mergeWith(const JZUndoCommand* other) override
    {
        const EditCommand* o = static_cast<const EditCommand*>(other);
        for (const Patch& p : o->m_patches)
            m_patches.push_back(p);
        m_timeMs = o->m_timeMs;         // 时间窗随最新编辑滚动
        m_cursorAfter = o->m_cursorAfter;
        m_anchorAfter = o->m_anchorAfter;
    }

    void undo() override
    {
        for (int i = static_cast<int>(m_patches.size()) - 1; i >= 0; --i) {
            const Patch& p = m_patches[static_cast<size_t>(i)];
            m_owner->applyEditRangeRaw(p.pos, static_cast<int>(p.inserted.size()),
                                       p.removed);
        }
        m_owner->finishEditRange(m_cursorBefore, m_anchorBefore);
    }

    void redo() override
    {
        for (const Patch& p : m_patches) {
            m_owner->applyEditRangeRaw(p.pos, static_cast<int>(p.removed.size()),
                                       p.inserted);
        }
        m_owner->finishEditRange(m_cursorAfter, m_anchorAfter);
    }

private:
    JZAbstractTextEdit* m_owner = nullptr;
    std::vector<Patch> m_patches;
    bool m_isolated = false;            // true = 粘贴等独立操作, 不与相邻命令合并
    long long m_timeMs = 0;             // 最近一次编辑的单调时钟毫秒
    int m_cursorBefore = 0, m_anchorBefore = 0;
    int m_cursorAfter  = 0, m_anchorAfter  = 0;
};

JZ_OBJECT_IMPL(JZAbstractTextEdit, JZWidget)

JZAbstractTextEdit::JZAbstractTextEdit(JZWidget* parent, bool singleLine)
    : JZWidget(parent)
    , sigTextChanged(this)
    , sigCursorPositionChanged(this)
    , sigReturnPressed(this)
    , sigEditingFinished(this)
    , m_singleLine(singleLine)
{
    m_cursorTimer = new JZTimer(this);
    m_cursorTimer->setSingleShot(false);
    connect(&m_cursorTimer->sigTimeout, this, &JZAbstractTextEdit::onCursorTimer);

    m_undoStack = new JZUndoStack;
    relayout();
}

JZAbstractTextEdit::~JZAbstractTextEdit()
{
    // 注: 输入上下文 (IMC) 是 HWND 级的, 由所属窗口统一关联/断开
    // (JZWindow::updateImeContext), 控件析构时不能去动它 —— 否则会把同窗口
    // 其它输入框的输入法一起关掉。
    delete m_undoStack;     // 释放全部撤销历史 (命令的 undo/redo 不再被调用)
    m_undoStack = nullptr;
}

// ==================== 属性 ====================

void JZAbstractTextEdit::setText(const std::string& text)
{
    JZString next = JZString::fromStdString(text);
    if (m_singleLine)
        next = flattenNewlines(next);
    if (m_maxLength > 0 && next.size() > m_maxLength)
        next = next.left(m_maxLength);

    // 程序整体设值会令旧编辑历史失效 (Qt setText 同样清空 undo/redo);
    // 文本未变时保留历史, 避免周期性 setText 意外打断用户输入
    if (!(m_text == next))
        m_undoStack->clear();

    m_text = next;
    m_cursor = m_anchor = m_text.size();
    m_scrollX = m_scrollY = 0;
    m_desiredColumn = 0;
    relayout();
    ensureCursorVisible();
    update();
    sigTextChanged.emit(text);
}

std::string JZAbstractTextEdit::text() const
{
    return m_text.toStdString();
}

void JZAbstractTextEdit::setPlaceholderText(const std::string& text)
{
    m_placeholder = JZString::fromStdString(text);
    update();
}

std::string JZAbstractTextEdit::placeholderText() const
{
    return m_placeholder.toStdString();
}

void JZAbstractTextEdit::setReadOnly(bool readOnly)
{
    m_readOnly = readOnly;
    // 只读状态变化会影响 imeEnabled(), 让所属窗口重算 IMC 关联
    if (JZWindow* win = window())
        win->updateImeContext();
    update();
}

bool JZAbstractTextEdit::isReadOnly() const
{
    return m_readOnly;
}

// ==================== 撤销 / 重做 ====================

void JZAbstractTextEdit::undo()
{
    if (m_readOnly)
        return;
    m_undoStack->undo();
}

void JZAbstractTextEdit::redo()
{
    if (m_readOnly)
        return;
    m_undoStack->redo();
}

bool JZAbstractTextEdit::canUndo() const
{
    return !m_readOnly && m_undoStack && m_undoStack->canUndo();
}

bool JZAbstractTextEdit::canRedo() const
{
    return !m_readOnly && m_undoStack && m_undoStack->canRedo();
}

void JZAbstractTextEdit::setUndoLimit(int limit)
{
    m_undoStack->setUndoLimit(limit);
}

int JZAbstractTextEdit::undoLimit() const
{
    return m_undoStack->undoLimit();
}

void JZAbstractTextEdit::clearUndoHistory()
{
    m_undoStack->clear();
}

bool JZAbstractTextEdit::imeEnabled() const
{
    // 密码框不接输入法: 组字串会明文出现在候选窗 / 系统组字窗里
    return !m_readOnly && !m_password;
}

JZSize JZAbstractTextEdit::minimumSizeHint() const
{
    // 文本框最小尺寸: 宽度允许收缩到 0 (内容裁切), 高度给出行高下限。
    // 单行: 一行文本高; 多行: 至少两行可见。
    return JZSize(0, m_singleLine ? 24 : 48);
}

void JZAbstractTextEdit::setPasswordMode(bool password)
{
    m_password = password;
    update();
}

void JZAbstractTextEdit::setMaxLengthLimit(int n)
{
    m_maxLength = n;
    if (m_maxLength > 0 && m_text.size() > m_maxLength) {
        m_text = m_text.left(m_maxLength);
        m_undoStack->clear();       // 文本被外力截断, 历史补丁不再一致
    }
}

void JZAbstractTextEdit::setLineWrapMode(bool wrap)
{
    m_lineWrap = wrap;
    relayout();
    update();
}

void JZAbstractTextEdit::setCursorBlinkPeriod(int ms)
{
    m_blinkPeriod = ms;
    if (m_cursorTimer->isActive()) {
        m_cursorTimer->stop();
        if (ms > 0)
            m_cursorTimer->start(ms);
    }
}

void JZAbstractTextEdit::setTextMargins(int left, int top, int right, int bottom)
{
    m_marginLeft = left;
    m_marginTop = top;
    m_marginRight = right;
    m_marginBottom = bottom;
    relayout();
    ensureCursorVisible();
    update();
}

void JZAbstractTextEdit::setTextMargins(int left, int right)
{
    m_marginLeft = left;
    m_marginRight = right;
    ensureCursorVisible();
    update();
}

// ==================== 行布局 ====================

int JZAbstractTextEdit::lineHeight() const
{
    JZFont font(kFontPixelSize);
    return JZFontMetrics::textHeight(font) + kLineGap;
}

// 文本 -> 显示文本: 密码模式把每个码点替换成 '*'
// 注意**所有**宽度测量 (光标偏移 / 点击定位 / IME 锚点) 都必须走它,
// 否则密码模式下光标、命中点与 '*' 的实际显示位置越输越偏。
JZString JZAbstractTextEdit::displayString(const JZString& s) const
{
    if (!m_password)
        return s;

    JZString out;
    for (int i = 0; i < s.size(); ++i)
        out.push_back(JZChar('*'));
    return out;
}

// 重建行布局。单行恒为一行 (文本入口已清洗换行); 多行按 '\n' 切分。
void JZAbstractTextEdit::relayout()
{
    m_lines.clear();

    const int n = m_text.size();

    if (m_singleLine) {
        LineLayout L;
        L.start = 0;
        L.length = n;
        L.y = 0;
        m_lines.push_back(L);
        return;
    }

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

int JZAbstractTextEdit::lineOfIndex(int idx) const
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

void JZAbstractTextEdit::locateIndex(int idx, int& line, int& column) const
{
    line = lineOfIndex(idx);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    column = idx - L.start;
}

int JZAbstractTextEdit::indexFromLineColumn(int line, int column) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    return L.start + std::max(0, std::min(column, L.length));
}

// 首行文本区顶 y (控件坐标)。多行 = marginTop - scrollY;
// 单行 = 把"行块"在控件高度内垂直居中, 该公式与逐像素居中一致。
int JZAbstractTextEdit::contentTop() const
{
    if (m_singleLine)
        return (height() - lineHeight()) / 2;
    return m_marginTop - m_scrollY;
}

// 行内 x -> 列号 (取最近的字符边界; 密码模式按掩码行文本测宽)
int JZAbstractTextEdit::columnFromX(int line, int x) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    JZFont font(kFontPixelSize);
    const JZString lineText = displayString(m_text.mid(L.start, L.length));
    const std::string u8 = lineText.toStdString();
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

// 客户区坐标 -> 文本下标。单行模式下任何 y 都命中唯一的行, 与旧的
// LineEdit::indexFromX 等价; 多行按行 y 区间命中。
int JZAbstractTextEdit::indexFromXY(int x, int y) const
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

int JZAbstractTextEdit::cursorPosition() const
{
    return m_cursor;
}

int JZAbstractTextEdit::selStart() const
{
    return std::min(m_cursor, m_anchor);
}

int JZAbstractTextEdit::selEnd() const
{
    return std::max(m_cursor, m_anchor);
}

bool JZAbstractTextEdit::hasSelection() const
{
    return m_cursor != m_anchor;
}

int JZAbstractTextEdit::selectionStart() const
{
    return selStart();
}

int JZAbstractTextEdit::selectionLength() const
{
    return selEnd() - selStart();
}

std::string JZAbstractTextEdit::selectedText() const
{
    if (!hasSelection())
        return std::string();
    return m_text.mid(selStart(), selectionLength()).toStdString();
}

void JZAbstractTextEdit::setCursorPosition(int pos)
{
    const int n = m_text.size();
    pos = std::max(0, std::min(pos, n));
    if (m_cursor == pos && m_anchor == pos)
        return;

    m_cursor = m_anchor = pos;
    refreshDesiredColumn();
    ensureCursorVisible();
    resetCursorBlink();
    update();
    sigCursorPositionChanged.emit();
}

void JZAbstractTextEdit::selectAll()
{
    m_anchor = 0;
    m_cursor = m_text.size();
    ensureCursorVisible();
    update();
}

void JZAbstractTextEdit::clearSelection()
{
    m_anchor = m_cursor;
    update();
}

void JZAbstractTextEdit::setSelection(int start, int length)
{
    const int n = m_text.size();
    const int s = std::max(0, std::min(start, n));
    const int e = std::max(s, std::min(start + length, n));
    m_anchor = s;
    m_cursor = e;
    ensureCursorVisible();
    update();
}

// 把 [pos, pos+len) 替换为 after, 作为一步撤销命令提交 (push 会立即 redo)。
// isolated = true 表示独立命令 (粘贴), 不与前后编辑合并。
void JZAbstractTextEdit::replaceRangeWithUndo(int pos, int removeLen,
                                              const JZString& after, bool isolated)
{
    pos = std::max(0, std::min(pos, m_text.size()));
    removeLen = std::max(0, std::min(removeLen, m_text.size() - pos));
    const JZString removed = m_text.mid(pos, removeLen);
    if (removed.isEmpty() && after.isEmpty())
        return;                         // 无实际变化

    const int beforeCursor = m_cursor;
    const int beforeAnchor = m_anchor;
    const int afterCursor  = pos + after.size();
    const long long now = nowMs();

    EditCommand* cmd = new EditCommand(this, pos, removed, after, isolated, now,
                                       beforeCursor, beforeAnchor,
                                       afterCursor, afterCursor);
    m_undoStack->push(cmd);             // push 内部执行 redo(): 文本已改
}

// 撤销命令内部调用: 只改文本数据, 不动光标/不发信号 (由 finishEditRange 收尾)
void JZAbstractTextEdit::applyEditRangeRaw(int pos, int removeLen,
                                           const JZString& after)
{
    m_text.replace(pos, removeLen, after);
}

// 撤销/重做后的统一收尾: 光标 -> 重建行布局/滚动/重绘/IME 定位/信号
void JZAbstractTextEdit::finishEditRange(int cursor, int anchor)
{
    m_cursor = std::max(0, std::min(cursor, m_text.size()));
    m_anchor = std::max(0, std::min(anchor, m_text.size()));
    afterContentChanged();
    refreshDesiredColumn();
}

void JZAbstractTextEdit::deleteSelection()
{
    if (!hasSelection())
        return;
    const int s = selStart();
    replaceRangeWithUndo(s, selEnd() - s, JZString(), false);
}

// 上屏文字的唯一入口: 替换选区 -> 插入 (整体算一步撤销)。
// isolated = true 时作为独立命令 (粘贴), 不与相邻输入合并。
void JZAbstractTextEdit::insertText(const JZString& s, bool isolated)
{
    if (s.isEmpty())
        return;

    const bool sel = hasSelection();
    const int start = sel ? selStart() : m_cursor;
    const int removeLen = sel ? selectionLength() : 0;

    JZString ins = s;
    if (m_singleLine)
        ins = flattenNewlines(ins);
    if (m_maxLength > 0) {
        const int room = m_maxLength - (m_text.size() - removeLen);
        if (room <= 0)
            return;
        if (ins.size() > room)
            ins = ins.left(room);
    }
    if (ins.isEmpty())
        return;

    replaceRangeWithUndo(start, removeLen, ins, isolated);
}

// 注意: 这里不更新 m_desiredColumn —— 上下移动时要沿用记忆列,
// 由水平移动的调用方负责刷新它。
void JZAbstractTextEdit::moveCursor(int pos, bool select)
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

// 上下移动: 用记忆列 m_desiredColumn 定位, 短行自动靠到行尾 (仅多行)
void JZAbstractTextEdit::moveCursorVertical(int deltaLine, bool select)
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);

    const int target = std::max(0, std::min(line + deltaLine,
                                            static_cast<int>(m_lines.size()) - 1));
    moveCursor(indexFromLineColumn(target, m_desiredColumn), select);
}

void JZAbstractTextEdit::selectWordAt(int pos)
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

void JZAbstractTextEdit::refreshDesiredColumn()
{
    if (m_lines.empty())
        return;
    m_desiredColumn = m_cursor - m_lines[static_cast<size_t>(lineOfIndex(m_cursor))].start;
}

void JZAbstractTextEdit::ensureCursorVisible()
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    // 垂直滚动 (仅多行; 单行不滚动)
    if (!m_singleLine) {
        const int viewH = height() - m_marginTop - m_marginBottom;
        if (viewH > 0) {
            if (L.y - m_scrollY < 0)
                m_scrollY = L.y;
            else if (L.y + lineHeight() - m_scrollY > viewH)
                m_scrollY = L.y + lineHeight() - viewH;
        }
    }

    // 水平滚动 (组字时光标落在组字串内, 要算上组字前缀宽度; 密码模式按掩码测宽)
    const int viewW = width() - m_marginLeft - m_marginRight;
    if (viewW > 0) {
        JZFont font(kFontPixelSize);
        const JZString lineText = displayString(m_text.mid(L.start, L.length));
        int cx = JZFontMetrics::textWidth(lineText.toStdString(), font, column);
        if (m_composing && !m_comp.isEmpty()) {
            const int n = std::max(0, std::min(m_compCursor, m_comp.size()));
            cx += JZFontMetrics::textWidth(
                      displayString(m_comp).toStdString(), font, n);
        }

        if (cx - m_scrollX < 0)
            m_scrollX = std::max(0, cx - 8);
        else if (cx - m_scrollX > viewW)
            m_scrollX = cx - viewW + 8;
    }
}

void JZAbstractTextEdit::resetCursorBlink()
{
    m_cursorOn = true;
    m_cursorTimer->stop();
    if (m_blinkPeriod > 0)
        m_cursorTimer->start(m_blinkPeriod);
}

void JZAbstractTextEdit::onCursorTimer()
{
    m_cursorOn = !m_cursorOn;
    update();
}

void JZAbstractTextEdit::afterContentChanged()
{
    relayout();
    ensureCursorVisible();
    resetCursorBlink();
    update();
    updateIMEWindowPos();
    sigTextChanged.emit(text());
}

// ==================== 尺寸变化 ====================

void JZAbstractTextEdit::resizeEvent(JZResizeEvent*)
{
    if (m_singleLine)
        return;     // 单行无需重排

    if (m_lineWrap)
        relayout();              // 自动换行: 宽度变化后行会重新切分
    ensureCursorVisible();       // 高度变化后光标行可能滚出可视区
}

// ==================== 绘制 ====================

void JZAbstractTextEdit::paintEvent(JZPainterEvent*)
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

    // 可视裁剪区: 多行 = marginTop..h-marginBottom; 单行贴近外框 (h 上下留 1px)
    const int clipTop    = m_singleLine ? 1 : m_marginTop;
    const int clipBottom = m_singleLine ? (h - 1) : (h - m_marginBottom);
    const int clipW = w - m_marginLeft - m_marginRight;
    const int clipH = clipBottom - clipTop;
    if (clipW <= 0 || clipH <= 0)
        return;
    p.setClip(JZRect(m_marginLeft, clipTop, clipW, clipH));

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

        // 行不在可视区则跳过 (单行不参与行裁剪)
        if (!m_singleLine &&
            (yLine + lh < m_marginTop || yLine > h - m_marginBottom))
            continue;

        const JZString lineText = displayString(m_text.mid(L.start, L.length));
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

        // 组字串画在光标所在行 (密码模式按掩码显示)
        if (i == compLine) {
            const int xComp = x0 + JZFontMetrics::textWidth(u8, font, compCol);
            drawComposition(p, xComp, yText, bTop, visH, displayString(m_comp));
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        }
    }

    // 光标
    if (focused && m_cursorOn && !m_readOnly) {
        int curLine = 0, curCol = 0;
        locateIndex(m_cursor, curLine, curCol);
        const LineLayout& L = m_lines[static_cast<size_t>(curLine)];

        const JZString lineText = displayString(m_text.mid(L.start, L.length));
        int cx = m_marginLeft - m_scrollX
               + JZFontMetrics::textWidth(lineText.toStdString(), font, curCol);
        if (m_composing && !m_comp.isEmpty()) {
            const int n = std::max(0, std::min(m_compCursor, m_comp.size()));
            cx += JZFontMetrics::textWidth(displayString(m_comp).toStdString(), font, n);
        }

        p.fillRect(JZRect(cx, top + L.y + pad, 2, visH), JZColor(0x10, 0x10, 0x10));
    }
}

// 组字串三遍绘制: 反显底色 -> 文字 -> 下划线。
// 顺序不能乱: 底色画在文字之后会盖掉字。
void JZAbstractTextEdit::drawComposition(JZPainter& p, int x, int textTop, int visTop, int visH,
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

void JZAbstractTextEdit::mousePressEvent(JZMouseEvent* event)
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

    // 自建双击检测: 不依赖消息层是否转发 WM_LBUTTONDBLCLK
    // (JZMouseEvent 目前没有 DoubleClick 类型)
    const bool isDoubleClick =
        (now - m_lastClickMs < 400) && (std::abs(x - m_lastClickX) <= 4);
    m_lastClickMs = now;
    m_lastClickX = x;

    if (isDoubleClick) {
        selectWordAt(indexFromXY(x, y));
    } else {
        m_cursor = m_anchor = indexFromXY(x, y);    // 普通点击: 清除选区
        refreshDesiredColumn();
        m_dragging = true;
        resetCursorBlink();
        update();
    }

    updateIMEWindowPos();
}

void JZAbstractTextEdit::mouseMoveEvent(JZMouseEvent* event)
{
    if (!m_dragging)
        return;

    const int pos = indexFromXY(event->localPos().x(), event->localPos().y());
    if (pos == m_cursor)
        return;

    m_cursor = pos;                     // 锚点不动 -> 拖动出选区 (可跨行)
    refreshDesiredColumn();
    ensureCursorVisible();
    resetCursorBlink();
    update();
}

void JZAbstractTextEdit::mouseReleaseEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        m_dragging = false;
}

// ==================== 键盘 ====================

void JZAbstractTextEdit::keyPressEvent(JZKeyEvent* event)
{
    const uint32_t key = event->key();
    const bool shift = event->shift();
    const bool ctrl  = event->ctrl();

    // 组字期间按键全部交给 IME: 方向键/退格作用于组字串内部, 本控件不处理
    if (m_composing && key != JZKey::Key_Tab) {
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
        case 'Z': if (shift) redo(); else undo();        event->accept(); return;
        case 'Y': redo();                                event->accept(); return;
        default: break;     // Ctrl+Home / Ctrl+End 落到下面的 switch
        }
    }

    switch (key) {
    case JZKey::Key_Left:
        if (shift)                 moveCursor(m_cursor - 1, true);
        else if (hasSelection())   moveCursor(selStart(), false);   // 先折叠到选区起点
        else                       moveCursor(m_cursor - 1, false);
        // 水平移动: 刷新记忆列
        refreshDesiredColumn();
        event->accept();
        return;

    case JZKey::Key_Right:
        if (shift)                 moveCursor(m_cursor + 1, true);
        else if (hasSelection())   moveCursor(selEnd(), false);     // 先折叠到选区终点
        else                       moveCursor(m_cursor + 1, false);
        refreshDesiredColumn();
        event->accept();
        return;

    case JZKey::Key_Up:
        if (m_singleLine) {
            event->ignore();        // 单行: 交回父级 (如 ScrollArea 滚动)
            return;
        }
        moveCursorVertical(-1, shift);
        event->accept();
        return;

    case JZKey::Key_Down:
        if (m_singleLine) {
            event->ignore();
            return;
        }
        moveCursorVertical(1, shift);
        event->accept();
        return;

    case JZKey::Key_Home: {
        int line = 0, col = 0;
        locateIndex(m_cursor, line, col);
        // 单行只有一行 -> 行首 == 文本首, 与 LineEdit 的 Home 语义一致
        const int target = ctrl ? 0 : m_lines[static_cast<size_t>(line)].start;
        moveCursor(target, shift);
        refreshDesiredColumn();
        event->accept();
        return;
    }

    case JZKey::Key_End: {
        int line = 0, col = 0;
        locateIndex(m_cursor, line, col);
        const LineLayout& L = m_lines[static_cast<size_t>(line)];
        const int target = ctrl ? m_text.size() : L.start + L.length;
        moveCursor(target, shift);
        refreshDesiredColumn();
        event->accept();
        return;
    }

    case JZKey::Key_Backspace:
        if (m_readOnly) break;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_cursor > 0) {
            replaceRangeWithUndo(m_cursor - 1, 1, JZString(), false);
        }
        event->accept();        // 收尾 (重排/光标/信号) 由撤销命令完成
        return;

    case JZKey::Key_Delete:
        if (m_readOnly) break;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_cursor < m_text.size()) {
            replaceRangeWithUndo(m_cursor, 1, JZString(), false);
        }
        event->accept();
        return;

    case JZKey::Key_Return:
        if (m_singleLine) {
            // 单行: 回车是"完成编辑" (即便只读也照发信号)
            sigReturnPressed.emit();
            sigEditingFinished.emit();
            event->accept();
            return;
        }
        if (m_readOnly) break;
        insertText(JZString("\n"));     // 多行: 回车是换行, 不是"完成编辑"
        m_desiredColumn = 0;
        event->accept();
        return;

    case JZKey::Key_Escape:
        if (hasSelection()) {
            clearSelection();
            event->accept();
            return;
        }
        break;      // 无选区时交回系统 / IME 处理

    case JZKey::Key_Tab:
        break;      // 交回焦点系统做控件切换

    default:
        break;
    }

    // 可打印字符 (IME 上屏的内容不走这里, 走 imeEvent 的 ResultStr)
    if (!ctrl && !m_readOnly && !event->text().empty()) {
        insertText(JZString::fromStdString(event->text()));
        refreshDesiredColumn();
        event->accept();
        return;
    }

    event->ignore();
}

// ==================== 输入法 ====================

void JZAbstractTextEdit::imeEvent(JZImeEvent* event)
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
        // (非 BMP 字符在 UTF-16 里占两个单元, 直接按下标拷贝会错位)
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
        // 光标位置同样是 UTF-16 下标。换算基准必须是当前组字串 (m_comp):
        // 单独 GCS_CURSORPOS 到来时 event->compString() 是空的, 用它换算会把
        // 光标打到 0, 组字串里的插入点看起来"跳回开头"。
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

void JZAbstractTextEdit::updateIMEWindowPos()
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

    // 锚点取**组字起点**而非当前光标: 组字时光标会在组字串里移动,
    // 若用光标位置, 候选条左边缘会跟着一路右移, 看起来就是"偏了"
    int line = 0, col = 0;
    locateIndex(selStart(), line, col);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    const JZString lineText = displayString(m_text.mid(L.start, L.length));
    const int startX = m_marginLeft - m_scrollX
        + JZFontMetrics::textWidth(lineText.toStdString(), font, col);
    const int startY = contentTop() + L.y + pad;

    // mapToGlobal: 控件坐标 -> 屏幕逻辑坐标 (内部已做 ×scale / ClientToScreen /
    // ÷scale), 与 Qt 一致; IME 候选条按此锚点摆放。
    const JZPoint sTop = mapToGlobal(JZPoint(startX, startY));
    const JZPoint sBot = mapToGlobal(JZPoint(startX, startY + visH));

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

    // 候选窗左上角: 锚在当前输入行(组字起点所在行)的正下方一行。
    // 用 CFS_CANDIDATEPOS (比 CFS_EXCLUDE 兼容性好): 微软拼音/搜狗会把候选窗左上角
    // 直接放到 ptCurrentPos, 即"正在输入的那一行"底下 —— 这正是"输入框应在输入文字下面"。
    // 注意: 多行输入框若当前行在框底(文本已填满), 候选窗会自然落在框底那一行下方,
    // 这是预期行为, 不是偏到框外。
    CANDIDATEFORM cdf;
    ZeroMemory(&cdf, sizeof(cdf));
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = pt.x    + toPhysical(kImeCandOffsetX, scale);
    cdf.ptCurrentPos.y = ptBot.y + toPhysical(kImeCandOffsetY, scale);
    ImmSetCandidateWindow(hIMC, &cdf);

    ImmReleaseContext(hwnd, hIMC);
}

void JZAbstractTextEdit::cancelComposition()
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

void JZAbstractTextEdit::completeComposition()
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

void JZAbstractTextEdit::focusInEvent(JZEvent*)
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

void JZAbstractTextEdit::focusOutEvent(JZEvent*)
{
    cancelComposition();          // 放弃未上屏的组字
    m_dragging = false;
    m_cursorTimer->stop();
    m_cursorOn = false;
    update();
    if (m_singleLine)
        sigEditingFinished.emit();   // 单行: 失焦即"编辑完成"
}

// ==================== 剪贴板 ====================

void JZAbstractTextEdit::copyToClipboard()
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

void JZAbstractTextEdit::cutToClipboard()
{
    if (m_readOnly || !hasSelection())
        return;

    copyToClipboard();
    // 剪切 = 复制 + 删除; 删除作独立一步撤销 (不与前后编辑合并)
    const int s = selStart();
    replaceRangeWithUndo(s, selEnd() - s, JZString(), true);
}

void JZAbstractTextEdit::pasteFromClipboard()
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

            if (!m_singleLine) {
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
                u8.swap(norm);
            }
            // 单行: 换行由 insertText 统一替换成空格

            // 粘贴作为独立一步撤销, 不与前后编辑合并 (isolated = true)
            insertText(JZString::fromStdString(u8), true);
            return;
        }
    }
    CloseClipboard();
}
