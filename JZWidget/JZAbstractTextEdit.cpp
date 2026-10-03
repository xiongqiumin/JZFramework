#include "JZAbstractTextEdit.h"

#include "JZUndoStack.h"
#include "JZTextImageMap.h"
#include "JZEmojiImageMap.h"
#include "JZUtf8Iterator.h"

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
#include <cstring>
#include <fstream>

// windows.h 会定义 min/max 宏, 不加这个 std::min/std::max 会被破坏
#define NOMINMAX
#include <windows.h>
#include <imm.h>

namespace {

const int kFontPixelSize = 14;      // 默认字号 (逻辑像素)
const int kLineGap       = 2;       // 行间距 (多行)

// 内联图片行高下限: 太小的图不压缩行高
const int kInlineImageMinH = 20;

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

// 求"像素宽度不超过 maxW 的最大码点数"。用于组字期间把与组字区重叠的文本
// 按像素宽度切成"可绘制前缀"。
int cpCountForWidth(const std::string& u8, const JZFont& font, int maxW)
{
    if (maxW <= 0 || u8.empty())
        return 0;

    int count = 0;
    while (true) {
        const int next = count + 1;
        const int w = JZFontMetrics::textWidth(u8, font, next);
        if (w <= 0 || w > maxW)
            break;
        count = next;
    }
    return count;
}

} // namespace

// ==================== 图文混排定位 (单码点占位) ====================
//
// 图片在 m_text 里就是一个**私有域码点** (JZTextImageMap::isImageCode),
// 于是"码点下标"同时就是光标/选区坐标: 不再需要视觉下标或 run 模型。
// 唯一特判是**测宽**: 普通码点走字体度量, 图片码点查注册表的显示宽。
// 所有 "部件序号 -> x 坐标" 的计算统一走 xOfIndex(), 不允许别处再做前缀测宽。

// 行首/行尾码点下标
void JZAbstractTextEdit::lineRange(int line, int& flatStart, int& flatEnd) const
{
    if (m_lines.empty()) {
        flatStart = flatEnd = 0;
        return;
    }
    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    flatStart = L.start;
    flatEnd = L.start + L.length;
}

// 单行内前 partCount 个部件的累计宽度 (相对行首, 不含 margin/scroll)。
// **唯一**允许计算光标/命中点 x 坐标的函数: 以下四处必须全部走它 ——
//   columnFromX / ensureCursorVisible / paintEvent 光标绘制 / updateIMEWindowPos
//
// 算法: 把行按**段落**切成 "Text 游程 + Image/Emoji 图", 文本游程整段前缀测宽
// (与 drawText 的排版一致), 图段宽度走 segmentImage (与绘制同一来源)。
int JZAbstractTextEdit::xOfIndex(int line, int partCount) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    int flatStart = 0, flatEnd = 0;
    lineRange(line, flatStart, flatEnd);

    partCount = std::max(0, std::min(partCount, flatEnd - flatStart));
    if (partCount == 0)
        return 0;

    JZFont font(kFontPixelSize);
    const int stop = flatStart + partCount;   // 只累计到这个码点之前

    // 快路径: 区间内没有图片/emoji ⇒ 整段就是一个 Text 段, 直接一次前缀测宽。
    // 与下面的通用路径结果完全一致 (通用路径此时也只会测这一个区间), 但省掉
    // enumerateSegments 的分配与逐段循环 —— 本函数在 paintEvent 里按行按段调用。
    if (!hasSpecialCode(flatStart, stop)) {
        const JZString sub = displayString(m_text.mid(flatStart, stop - flatStart));
        return JZFontMetrics::textWidth(sub.toStdString(), font);
    }

    // 本行的段落序列 (字节区间), 一次取出反复用
    const int byteStart = codepointToUtf8Byte(flatStart);
    const int byteStop  = codepointToUtf8Byte(stop);
    const std::vector<Segment> segs =
        enumerateSegments(byteStart, flatEnd == m_text.size()
                          ? -1 : codepointToUtf8Byte(flatEnd) - byteStart);

    int x = 0;
    for (size_t k = 0; k < segs.size(); ++k) {
        const Segment& s = segs[k];

        // 只统计 [flatStart, stop) 范围内的段落
        const int sCpBegin = utf8ByteToCodepoint(s.start);
        if (sCpBegin >= stop)
            break;
        const int sCpEnd = utf8ByteToCodepoint(s.start + s.size);

        if (s.type == SegmentType::Text) {
            // 文本段: 取与本区间相交的部分做前缀测宽
            const int a = std::max(sCpBegin, flatStart);
            const int b = std::min(sCpEnd, stop);
            if (b > a) {
                const JZString sub = displayString(m_text.mid(a, b - a));
                x += JZFontMetrics::textWidth(sub.toStdString(), font);
            }
            continue;
        }

        // Image / Emoji 段: 整格宽度 (与绘制同一来源)。
        // 只有该段**完整落在**统计范围内才计入 (光标按段停靠, 不会落在段中间)。
        if (sCpEnd <= stop) {
            const JZPixmap* pm = nullptr;
            int w = 0, h = 0;
            if (segmentImage(s, pm, w, h))
                x += w;
        }
    }
    return x;
}

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

// ==================== 图文混排: 图片插入 ====================
//
// 图片不占多字符标记, 而是在 m_text 里放**一个私有域码点**。像素/尺寸登记到
// 进程级单例 JZTextImageMap (按 key 幂等分配码点)。因为码点本身就是普通字符,
// 插入就是一次普通的文本插入 —— 撤销/光标/选区/信号全部复用既有链路。

// key 缺省时生成唯一的 "img_<n>" (需与注册表里已有 key 不冲突)
std::string JZAbstractTextEdit::makeImageKey(const std::string& key) const
{
    if (!key.empty())
        return key;

    JZTextImageMap& map = JZTextImageMap::instance();
    int n = m_nextImageKey;
    while (true) {
        const std::string cand = "img_" + std::to_string(n++);
        // 注册表按 key 幂等; 只要该 key 还没分配过码点即可用
        bool taken = false;
        // 遍历已有条目判断 key 是否占用 (条目数量级很小)
        for (char32_t cp = JZTextImageMap::kPuaBase;
             cp <= JZTextImageMap::kPuaEnd; ++cp) {
            const JZTextImageMap::Entry* e = map.find(cp);
            if (!e)
                break;                  // 码点已连续到空位, 后面不可能再有
            if (e->key == cand) { taken = true; break; }
        }
        if (!taken)
            return cand;
    }
}

void JZAbstractTextEdit::appendImage(const JZImage& image, const std::string& key,
                                     int width, int height)
{
    // 末尾 = 文本长度 (每个字符/图片都占 1 个下标)
    insertImageAt(m_text.size(), image, key, width, height);
}

void JZAbstractTextEdit::insertImage(const JZImage& image, const std::string& key,
                                     int width, int height)
{
    // 与 insertText 语义一致: 有选区先替换选区
    const bool sel = hasSelection();
    const int pos = sel ? selStart() : m_cursor;
    const int removeLen = sel ? (selEnd() - selStart()) : 0;

    if (removeLen > 0)
        replaceRangeWithUndo(pos, removeLen, JZString(), false);
    insertImageAt(pos, image, key, width, height);
}

// pos = 码点下标 (0..m_text.size()) 定位插入点
void JZAbstractTextEdit::insertImageAt(int pos, const JZImage& image,
                                       const std::string& key,
                                       int width, int height)
{
    if (m_readOnly)
        return;

    const std::string useKey = makeImageKey(key);
    const int w = width  > 0 ? width  : image.width();
    const int h = height > 0 ? height : image.height();
    if (useKey.empty() || (image.isNull() && (w <= 0 || h <= 0)))
        return;                         // 既无像素又无尺寸: 无法布局

    // 登记 (幂等): 取回私有域码点。同一 key 重复插入复用同一码点。
    const char32_t cp = JZTextImageMap::instance().add(useKey, image, w, h);
    if (cp == 0)
        return;

    // 单个码点作为普通文本插入 (isolated: 撤销时与相邻输入分开)。
    // 引用计数**不在这里**增减 —— 统一由 relayout() -> syncImageRefs()
    // 按"文本里实际出现了几次"对账, 避免与撤销/重做路径重复计数。
    JZString ins;
    ins.push_back(JZChar(cp));
    replaceRangeWithUndo(pos, 0, ins, true);
}

// 统计当前文本里出现的图片码点个数 (调试/测试用)
int JZAbstractTextEdit::inlineImageCount() const
{
    int n = 0;
    for (int i = 0; i < m_text.size(); ++i) {
        if (JZTextImageMap::isImageCode(m_text[i].toUnicode()))
            ++n;
    }
    return n;
}

// ==================== 段落 -> 可绘制图像 ====================
//
// 内联图片与 emoji 在渲染层面是**同一件事**: 都是"一张显示用位图 + 显示尺寸"。
// 因此测宽 (xOfIndex) 与绘制 (paintEvent) 都只调本函数, 不存在两套口径。
// 位图由两张注册表在入库/首次请求时**按显示尺寸**缩好并预转 (JZPixmap),
// 这里只是取指针, 无缩放、无格式转换。

// emoji 与图片的显示高度: 跟随字号 (略高于字形, 视觉上更平衡)
int JZAbstractTextEdit::inlineBoxHeight() const
{
    JZFont font(kFontPixelSize);
    return std::max(JZFontMetrics::textHeight(font) + 2, kInlineImageMinH);
}

bool JZAbstractTextEdit::segmentImage(const Segment& seg, const JZPixmap*& out,
                                      int& w, int& h) const
{
    out = nullptr;
    w = h = 0;

    if (seg.isImage()) {
        // 图片段: 码点在 Segment 里没有直接保留, 从段首字节回查
        const int cpIndex = utf8ByteToCodepoint(seg.start);
        if (cpIndex < 0 || cpIndex >= m_text.size())
            return false;

        const char32_t cp = m_text[cpIndex].toUnicode();
        if (!JZTextImageMap::isImageCode(cp))
            return false;

        // 显示尺寸在上限处 (JZTextImageMap::add) 就已定好, 这里只取结果
        JZTextImageMap::instance().sizeOf(cp, w, h);
        const JZTextImageMap::Entry* e = JZTextImageMap::instance().find(cp);
        if (e && !e->pixmap.isNull())
            out = &e->pixmap;
        return true;                     // 即使无像素也返回 true: 有确定尺寸
    }

    if (seg.isEmoji()) {
        // emoji 段: 查内置图集 (按显示框边长取已缩好的位图)。找不到 -> 降级。
        const int box = inlineBoxHeight();
        const JZPixmap* pm = JZEmojiImageMap::instance().findPixmap(seg.emoji, box);
        if (!pm || pm->isNull())
            return false;

        out = pm;
        w = pm->width();                 // == box (正方形)
        h = pm->height();
        return true;
    }

    return false;                        // Text 段不产生图像
}

// ==================== 段落语义 ====================
//
// 段落 = 光标移动 / 渲染 / 测宽 的共同口径。切分规则:
//   - 图片码点            -> 独立 Image 段 (不可分割);
//   - emoji 码点 (+修饰符) -> 独立 Emoji 段 (含 VS16/ZWJ/肤色/标签字符, 整体不可分割);
//   - 其余连续普通码点     -> 合并成一个 Text 段。
// 因此段落序列一定完整覆盖 [from, from+len) 的**字节区间**: 无空洞、无空段、无重叠。
//
// 坐标: Segment::start/size 用 **UTF-8 字节** (与 text()/setText() 同坐标系),
// 而控件内部光标/索引用**码点**。两者换算统一走 utf8ByteToCodepoint() 等辅助。

// ---- 文本字节视图缓存 ----
// 见头文件说明: 一次整串转码 + 建表, 之后所有 码点<->字节 换算都是查表/二分。
void JZAbstractTextEdit::rebuildByteCache() const
{
    m_u8Cache = m_text.toStdString();

    const int cpCount = m_text.size();
    const int byteCount = static_cast<int>(m_u8Cache.size());
    m_cpToByte.assign(static_cast<size_t>(cpCount) + 1, 0);

    int cp = 0;
    JZUtf8Iterator it(m_u8Cache);
    while (it.isValid() && cp < cpCount) {
        m_cpToByte[static_cast<size_t>(cp)] = it.byteIndex();
        ++cp;
        ++it;
    }
    // 非法字节会让迭代器提前跳到末尾: 剩余项统一填字节总长, 保证单调不减
    for (; cp <= cpCount; ++cp)
        m_cpToByte[static_cast<size_t>(cp)] = byteCount;
}

// 缓存与当前文本是否一致 (长度不符即视为失效, 回退到旧慢路径, 不返回错值)
bool JZAbstractTextEdit::byteCacheReady() const
{
    return !m_cpToByte.empty()
        && static_cast<int>(m_cpToByte.size()) == m_text.size() + 1;
}

bool JZAbstractTextEdit::hasSpecialCode(int cpFrom, int cpTo) const
{
    cpFrom = std::max(0, cpFrom);
    cpTo = std::min(cpTo, m_text.size());
    for (int i = cpFrom; i < cpTo; ++i) {
        const char32_t cp = m_text[i].toUnicode();
        if (JZTextImageMap::isImageCode(cp) || JZEmoji::isEmoji(cp))
            return true;
    }
    return false;
}

// UTF-8 字节下标 -> 码点下标 (相对整个 m_text)。不是码点起点时向下取到该码点。
int JZAbstractTextEdit::utf8ByteToCodepoint(int byteOffset) const
{
    if (byteOffset <= 0)
        return 0;

    if (byteCacheReady()) {
        // 第一个"起点字节 >= byteOffset"的码点 (与旧实现语义逐值一致)
        const std::vector<int>::const_iterator it =
            std::lower_bound(m_cpToByte.begin(), m_cpToByte.end(), byteOffset);
        return static_cast<int>(it - m_cpToByte.begin());
    }

    const std::string u8 = m_text.toStdString();
    JZUtf8Iterator it(u8);
    int cpIndex = 0;
    while (it.isValid()) {
        if (it.byteIndex() >= byteOffset)
            return cpIndex;
        ++cpIndex;
        ++it;
    }
    return cpIndex;                       // 到末尾
}

// 码点下标 -> UTF-8 字节下标 (相对整个 m_text)
int JZAbstractTextEdit::codepointToUtf8Byte(int cpIndex) const
{
    if (cpIndex <= 0)
        return 0;

    if (byteCacheReady())
        return (cpIndex >= m_text.size())
            ? static_cast<int>(m_u8Cache.size())
            : m_cpToByte[static_cast<size_t>(cpIndex)];

    const std::string u8 = m_text.toStdString();
    JZUtf8Iterator it(u8);
    int n = 0;
    while (it.isValid()) {
        if (n == cpIndex)
            return it.byteIndex();
        ++n;
        ++it;
    }
    return static_cast<int>(u8.size());   // 到末尾
}

// 把 [from, from+len) 的字节区间切成段落序列 (见头文件注释)。
std::vector<JZAbstractTextEdit::Segment>
JZAbstractTextEdit::enumerateSegments(int from, int len) const
{
    std::vector<Segment> segs;

    // 走缓存镜像: 本函数被按行调用, 每行再整串 toStdString() 是 O(行数×字符数)
    const bool cached = byteCacheReady();
    const std::string fallback = cached ? std::string() : m_text.toStdString();
    const std::string& u8 = cached ? m_u8Cache : fallback;
    const int n = static_cast<int>(u8.size());

    from = std::max(0, std::min(from, n));
    const int end = (len < 0) ? n : std::max(from, std::min(from + len, n));

    int i = from;
    while (i < end) {
        char32_t cp = 0;
        int cpLen = 0;
        if (!JZUtf8Iterator::decode(u8.data(), u8.size(), static_cast<size_t>(i),
                                    cp, cpLen) || cpLen <= 0)
            cpLen = 1;                     // 非法字节: 按 1 字节推进, 不丢内容
        if (i + cpLen > end)
            cpLen = end - i;               // 不越过区间右界

        // ---- 图片 ----
        if (JZTextImageMap::isImageCode(cp)) {
            Segment s;
            s.type = SegmentType::Image;
            s.start = i;
            s.size = cpLen;
            const JZTextImageMap::Entry* e = JZTextImageMap::instance().find(cp);
            if (e)
                s.image = e->image;        // 可能为空 (只有尺寸的占位)
            segs.push_back(s);
            i += cpLen;
            continue;
        }

        // ---- emoji (含其后的修饰符, 组合成一个不可分割的段) ----
        if (JZEmoji::isEmoji(cp)) {
            const int segStart = i;
            int j = i + cpLen;
            // 吸收后续的 VS16/ZWJ/肤色/标签字符 (ZWJ 后面还要再吸一个 emoji)
            while (j < end) {
                char32_t nc = 0;
                int nl = 0;
                if (!JZUtf8Iterator::decode(u8.data(), u8.size(),
                                            static_cast<size_t>(j), nc, nl) || nl <= 0)
                    break;
                if (j + nl > end)
                    break;
                if (JZEmoji::isEmojiModifier(nc)) {
                    j += nl;
                    continue;              // 修饰符并入本段
                }
                // ZWJ 之后的 emoji 已由上面吸收, 这里再容忍一个 emoji
                if (JZEmoji::isEmoji(nc)) {
                    j += nl;
                    continue;
                }
                break;
            }

            Segment s;
            s.type = SegmentType::Emoji;
            s.start = segStart;
            s.size = j - segStart;
            s.emoji = u8.substr(static_cast<size_t>(segStart),
                                static_cast<size_t>(s.size));
            segs.push_back(s);
            i = j;
            continue;
        }

        // ---- 普通文字: 一路吃到下一个图片/emoji (或区间末尾) ----
        const int segStart = i;
        int j = i + cpLen;
        while (j < end) {
            char32_t nc = 0;
            int nl = 0;
            if (!JZUtf8Iterator::decode(u8.data(), u8.size(),
                                        static_cast<size_t>(j), nc, nl) || nl <= 0)
                break;
            if (j + nl > end)
                break;
            if (JZTextImageMap::isImageCode(nc) || JZEmoji::isEmoji(nc))
                break;
            j += nl;
        }

        Segment s;
        s.type = SegmentType::Text;
        s.start = segStart;
        s.size = j - segStart;
        s.text = u8.substr(static_cast<size_t>(segStart), static_cast<size_t>(s.size));
        segs.push_back(s);
        i = j;
    }
    return segs;
}

// 段落起点的**码点下标**列表, 末尾额外追加哨兵 (== 末尾码点下标),
// 代表"最后一段之后的插入点"。可停靠位置数 = 段数 + 1。
std::vector<int> JZAbstractTextEdit::segmentStarts(int from, int len) const
{
    std::vector<int> starts;
    const int cpEnd = m_text.size();

    const std::vector<Segment> segs = enumerateSegments(from, len);
    for (size_t k = 0; k < segs.size(); ++k)
        starts.push_back(utf8ByteToCodepoint(segs[k].start));
    starts.push_back(cpEnd);             // 末尾插入点哨兵
    return starts;
}

int JZAbstractTextEdit::segmentIndexOfCursor(int pos) const
{
    if (pos < 0)
        pos = m_cursor;

    const std::vector<int> starts = segmentStarts();
    for (int k = 0; k < static_cast<int>(starts.size()); ++k) {
        if (pos < starts[k])
            return std::max(0, k - 1);
        if (pos == starts[k])
            return k;
    }
    return static_cast<int>(starts.size()) - 1;
}

bool JZAbstractTextEdit::moveCursorBySegment(int dir, bool select)
{
    if (dir == 0)
        return false;

    const std::vector<int> starts = segmentStarts();

    // 当前所在的停靠位置: 取"不大于光标的最后一个起点"
    int cur = 0;
    for (int k = 0; k < static_cast<int>(starts.size()); ++k) {
        if (starts[k] <= m_cursor)
            cur = k;
        else
            break;
    }

    const int next = cur + dir;
    if (next < 0 || next >= static_cast<int>(starts.size()))
        return false;                    // 已在首/末段落, 不再移动

    moveCursor(starts[next], select);
    return true;
}

void JZAbstractTextEdit::moveCursorToSegment(int idx, bool select)
{
    const std::vector<int> starts = segmentStarts();
    idx = std::max(0, std::min(idx, static_cast<int>(starts.size()) - 1));
    moveCursor(starts[static_cast<size_t>(idx)], select);
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
        rebuildByteCache();         // 文本被截断: 字节缓存必须同步重建
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

// 该行实际行高: 无图片时 == lineHeight(); 有图片取"行高 与 图片高+2"的较大者。
int JZAbstractTextEdit::lineHeightOf(int line) const
{
    if (m_lines.empty())
        return lineHeight();
    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    return m_lines[static_cast<size_t>(line)].height;
}

// 重建行布局。单行恒为一行 (文本入口已清洗换行); 多行按 '\n' 切分。
// 行高 = 该行所有 run 的高度峰值 (图片可撑高整行), 但以 lineHeight() 为下限,
// 保证纯文本时 y 累加结果与历史实现逐像素一致。
void JZAbstractTextEdit::relayout()
{
    m_lines.clear();
    // 本函数是所有文本变化路径的统一收口 (setText / 编辑 / 撤销 / 粘贴 / 图片插入
    // 都经 afterContentChanged 到这里), 因此字节视图缓存也在这里重建。
    rebuildByteCache();
    syncImageRefs();        // 文本变了 -> 同步图片引用计数 (先于布局)

    const int n = m_text.size();
    const int baseH = lineHeight();

    if (m_singleLine) {
        LineLayout L;
        L.start = 0;
        L.length = n;
        L.y = 0;
        L.height = lineHeightOfParts(0, n, baseH);
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
        L.height = lineHeightOfParts(L.start, L.start + L.length, baseH);
        m_lines.push_back(L);

        y += L.height;
        if (eol >= n)
            break;
        idx = eol + 1;      // 跳过 '\n'
    }
}

// 扫描 m_text, 把出现/消失的图片码点在 JZTextImageMap 里做引用计数增减。
// 图片码点是"文本的一部分", 因此文本一改就要同步记账 —— 挂在 relayout() 里,
// 保证 setText / 编辑 / 撤销 / 粘贴 全部覆盖 (它们最终都会重建行布局)。
void JZAbstractTextEdit::syncImageRefs()
{
    // 统计本次文本里各图片码点出现次数
    std::map<char32_t, int> seen;
    for (int i = 0; i < m_text.size(); ++i) {
        const char32_t cp = m_text[i].toUnicode();
        if (JZTextImageMap::isImageCode(cp))
            ++seen[cp];
    }

    // 与上次快照 diff: 多了 retain, 少了 release
    for (std::map<char32_t, int>::const_iterator it = seen.begin();
         it != seen.end(); ++it) {
        std::map<char32_t, int>::const_iterator old = m_imageRefs.find(it->first);
        const int prev = (old == m_imageRefs.end()) ? 0 : old->second;
        for (int k = prev; k < it->second; ++k)
            JZTextImageMap::instance().retain(it->first);
        for (int k = it->second; k < prev; ++k)
            JZTextImageMap::instance().release(it->first);
    }
    for (std::map<char32_t, int>::const_iterator it = m_imageRefs.begin();
         it != m_imageRefs.end(); ++it) {
        if (seen.find(it->first) != seen.end())
            continue;
        for (int k = 0; k < it->second; ++k)
            JZTextImageMap::instance().release(it->first);
    }
    m_imageRefs.swap(seen);
}

// 行高 = max(基准行高, 本行最大"图段"高 + 2)。图片/emoji 都可撑高整行。
int JZAbstractTextEdit::lineHeightOfParts(int flatStart, int flatEnd, int baseH) const
{
    int h = baseH;
    flatEnd = std::min(flatEnd, m_text.size());

    // 纯文本行: 行高就是基准行高, 不必枚举段落 (枚举要分配 vector + 拷贝子串)
    if (!hasSpecialCode(flatStart, flatEnd))
        return h;

    const int byteStart = codepointToUtf8Byte(std::max(0, flatStart));
    const int byteLen = codepointToUtf8Byte(flatEnd) - byteStart;
    const std::vector<Segment> segs = enumerateSegments(byteStart, byteLen);

    for (size_t k = 0; k < segs.size(); ++k) {
        if (segs[k].type == SegmentType::Text)
            continue;
        const JZPixmap* pm = nullptr;
        int w = 0, ih = 0;
        if (segmentImage(segs[k], pm, w, ih))
            h = std::max(h, ih + 2);
    }
    return h;
}

// 参数为**码点下标** (与 m_cursor 同一坐标系)
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

// 入参 idx 为码点下标; 输出 column 为**行内下标** (行内第几个部件)
void JZAbstractTextEdit::locateIndex(int idx, int& line, int& column) const
{
    line = lineOfIndex(idx);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];
    column = idx - L.start;
}

// 入参 (line, column): column 为行内下标; 返回码点下标
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

// 行内 x -> 视觉列号 (取最近的下标边界)。图文混排下必须按 run 走:
//   纯文本 run -> 原有"最近字符边界"逻辑 (逐像素一致);
//   图片部件 -> 命中左半返回其左边界, 右半返回右边界 (占 1 个下标)。
int JZAbstractTextEdit::columnFromX(int line, int x) const
{
    if (m_lines.empty())
        return 0;

    line = std::max(0, std::min(line, static_cast<int>(m_lines.size()) - 1));
    int flatStart = 0, flatEnd = 0;
    lineRange(line, flatStart, flatEnd);

    const int lineLen = flatEnd - flatStart;
    if (lineLen <= 0)
        return 0;

    JZFont font(kFontPixelSize);
    const int rel = x - (m_marginLeft - m_scrollX);

    // 逐部件取"该部件右边的累计宽度"与点击点比较, 取最近边界作为命中下标。
    // 单行部件数很小, 直接调 xOfIndex 即可 (图片/文本的宽度差异已封装在它内部)。
    int best = 0;
    int bestDist = INT_MAX;

    for (int i = 0; i <= lineLen; ++i) {
        const int w = xOfIndex(line, i);
        const int d = (w > rel) ? (w - rel) : (rel - w);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// 客户区坐标 -> 文本下标 (视觉)。单行模式下任何 y 都命中唯一的行, 与旧的
// LineEdit::indexFromX 等价; 多行按各行实际高度 (图片可撑高) 命中。
int JZAbstractTextEdit::indexFromXY(int x, int y) const
{
    if (m_lines.empty())
        return 0;

    const int top = contentTop();

    int line = 0;
    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
        line = i;
        const LineLayout& L = m_lines[static_cast<size_t>(i)];
        if (y < top + L.y + L.height)
            break;
    }
    return indexFromLineColumn(line, columnFromX(line, x));
}

// ==================== 光标与选区 ====================
//
// 坐标约定 (单码点占位, 2026-09-10): 图片就是一个码点, 所以 **码点下标**
// 就是唯一的坐标 —— 光标/选区/布局/命中/撤销补丁全部用它, 无需任何换算。
// (历史遗留的 flat/visual 双坐标已随 run 模型一起删除。)

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
    // 取子串按码点 (图片码点原样带出, 复制后仍指向同一张图)
    return m_text.mid(selStart(), selEnd() - selStart()).toStdString();
}

void JZAbstractTextEdit::setCursorPosition(int pos)
{
    pos = std::max(0, std::min(pos, m_text.size()));
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

// 入参为码点下标与长度 (图片码点同样算 1)
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

    // 注意: 这里必须用 selEnd()-selStart() (flat 长度), **不能**用
    // selectionLength() —— 后者返回视觉长度, 含图片标记时会短于实际码点数,
    // 导致替换选区时残留半截标记。
    const bool sel = hasSelection();
    const int start = sel ? selStart() : m_cursor;
    const int removeLen = sel ? (selEnd() - selStart()) : 0;

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
// pos 为**码点下标 (flat)** (由 indexFromLineColumn / visualToFlat 给出)
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

// 左右键的"一步": 图片就是一个码点, 因此 ±1 即天然跨过整张图。
// (旧的视觉单元步进已随 run 模型删除。)
int JZAbstractTextEdit::stepVisual(int dir) const
{
    return m_cursor + dir;
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

// m_desiredColumn 是**视觉**列 (上下移动时按视觉列对齐, 图片算 1 格)
void JZAbstractTextEdit::refreshDesiredColumn()
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);
    m_desiredColumn = column;
}

void JZAbstractTextEdit::ensureCursorVisible()
{
    if (m_lines.empty())
        return;

    int line = 0, column = 0;
    locateIndex(m_cursor, line, column);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    // 垂直滚动 (仅多行; 单行不滚动)。用该行实际行高 (图片可撑高)。
    if (!m_singleLine) {
        const int viewH = height() - m_marginTop - m_marginBottom;
        if (viewH > 0) {
            if (L.y - m_scrollY < 0)
                m_scrollY = L.y;
            else if (L.y + L.height - m_scrollY > viewH)
                m_scrollY = L.y + L.height - viewH;
        }
    }

    // 水平滚动。**必须走 xOfIndex** (含图片宽度);
    // 组字时光标落在组字串内, 还要算上组字前缀宽度。
    const int viewW = width() - m_marginLeft - m_marginRight;
    if (viewW > 0) {
        JZFont font(kFontPixelSize);
        int cx = xOfIndex(line, column);
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
    // 选区绘制按**视觉**下标与 run 比较 (ssV/seV 用行内视觉坐标, 见循环内换算)
    const int ss = selStart();
    const int se = selEnd();

    int compLine = -1, compCol = 0;
    JZString dispComp;
    int compW = 0;                 // 组字串总宽 (覆盖区宽度)
    if (m_composing && !m_comp.isEmpty()) {
        locateIndex(m_cursor, compLine, compCol);
        dispComp = displayString(m_comp);
        compW = JZFontMetrics::textWidth(dispComp.toStdString(), font);
    }
    // 组字串在行内的覆盖区间 [compX, compX + compW): 该区间内的既有文本不再绘制,
    // 避免组字文字与原文重叠重影; 图片则先铺底色再让组字文字叠上去。
    const int compX = (compLine >= 0)
        ? (m_marginLeft - m_scrollX + xOfIndex(compLine, compCol))
        : 0;

    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i) {
        const LineLayout& L = m_lines[static_cast<size_t>(i)];
        const int yLine = top + L.y;
        const int bTop  = yLine + pad;              // 字形可视顶
        const int yText = yLine + pad - lead;       // 文字 cell 顶

        // 行不在可视区则跳过 (单行不参与行裁剪)
        if (!m_singleLine &&
            (yLine + L.height < m_marginTop || yLine > h - m_marginBottom))
            continue;

        const int x0 = m_marginLeft - m_scrollX;
        const int lineEnd = L.start + L.length;

        // 选区在本行内的范围 (行内下标)
        const int ssIn = std::max(ss, L.start) - L.start;
        const int seIn = std::min(se, lineEnd) - L.start;

        // 组字覆盖区间 (仅组字所在行有效)
        const bool onCompLine = (i == compLine && compW > 0);
        const int compX0 = compX;
        const int compX1 = compX + compW;

        // 选区块底色: 连续区间的底色直接一次 fillRect (图片也算 1 格, 无需分段)
        if (selecting && seIn > ssIn) {
            const int xa = x0 + xOfIndex(i, ssIn);
            const int xb = x0 + xOfIndex(i, seIn);
            p.fillRect(JZRect(xa, bTop, xb - xa, visH), JZColor(0x33, 0x90, 0xFF));
        }

        // ---- 按段落绘制: Text 段落成批 drawText, Image/Emoji 段落 drawPixmap ----
        // 与 xOfIndex 用的是**同一个** enumerateSegments, 因此测宽与绘制同口径。
        const int lineByteStart = codepointToUtf8Byte(L.start);
        const int lineByteLen = codepointToUtf8Byte(L.start + L.length) - lineByteStart;
        const std::vector<Segment> segs = enumerateSegments(lineByteStart, lineByteLen);

        for (size_t k = 0; k < segs.size(); ++k) {
            const Segment& s = segs[k];
            const int sCpBegin = utf8ByteToCodepoint(s.start);          // 行内码点序号
            const int sCpLen   = utf8ByteToCodepoint(s.start + s.size) - sCpBegin;
            const int xs = x0 + xOfIndex(i, sCpBegin - L.start);

            // ---- 图段 (图片 / emoji): 同一路径 ----
            if (s.type != SegmentType::Text) {
                const JZPixmap* pm = nullptr;
                int iw = 0, ih = 0;
                if (segmentImage(s, pm, iw, ih)) {
                    const int iy = yLine + std::max(0, (L.height - ih) / 2);
                    if (pm && !pm->isNull()) {
                        // 位图已是显示尺寸且已转好: 这里是纯贴图 (唯一的 1:1 路径)
                        p.drawPixmap(*pm, xs, iy, iw, ih);
                    } else {
                        // 无像素 (只有尺寸): 浅灰占位框
                        p.fillRect(JZRect(xs, iy, iw, ih), JZColor(0xE4, 0xE7, 0xEC));
                        p.setPen(JZPen(JZColor(0xB0, 0xB6, 0xC0), 1));
                        p.drawRect(xs, iy, iw, ih);
                    }
                    // 与组字重叠时: 铺白底, 让随后的组字文字清晰叠上去
                    if (onCompLine && (xs + iw > compX0 && xs < compX1))
                        p.fillRect(JZRect(xs, yText - lead, iw, visH + lead),
                                   JZColor(0xFF, 0xFF, 0xFF));
                } else if (s.isEmoji()) {
                    // emoji 图集里没有这张图: 退回主字体绘制该 emoji 文本
                    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
                    p.drawText(xs, yText, s.emoji.c_str());
                }
                continue;
            }

            // ---- 文本段 ----
            const JZString segText = displayString(m_text.mid(sCpBegin, sCpLen));
            const std::string u8 = segText.toStdString();
            if (u8.empty())
                continue;

            if (!selecting) {
                // 组字期间: 整段落在覆盖区内则不画; 部分相交则只画左半前缀。
                // 测宽只在组字所在行才需要 —— 否则每段每帧白测一次 (长文本代价可观)
                const bool hitComp = onCompLine
                    && (xs + JZFontMetrics::textWidth(u8, font) > compX0)
                    && (xs < compX1);
                if (hitComp) {
                    if (compX0 > xs) {
                        const int cpCount = cpCountForWidth(u8, font, compX0 - xs);
                        if (cpCount > 0) {
                            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
                            p.drawText(xs, yText,
                                       segText.left(cpCount).toStdString().c_str());
                        }
                    }
                } else {
                    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
                    p.drawText(xs, yText, u8.c_str());
                }
                continue;
            }

            // 选区: 本段拆成 前(黑) / 内(白) / 后(黑) 三段
            const int segA = sCpBegin - L.start;
            const int segB = segA + sCpLen;
            const int a = std::max(ssIn, segA);
            const int b = std::min(seIn, segB);

            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
            if (a > segA)
                p.drawText(xs, yText, segText.left(a - segA).toStdString().c_str());

            if (b > a) {
                p.setPen(JZPen(JZColor(0xFF, 0xFF, 0xFF), 1));
                p.drawText(xs + JZFontMetrics::textWidth(u8, font, a - segA), yText,
                           segText.mid(a - segA, b - a).toStdString().c_str());
                p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
            }

            if (b < segB)
                p.drawText(xs + JZFontMetrics::textWidth(u8, font, b - segA), yText,
                           segText.mid(b - segA).toStdString().c_str());
        }

        // 组字串画在光标所在行 (密码模式按掩码显示); 起点走 xOfIndex 以兼容图片
        if (i == compLine && compW > 0) {
            drawComposition(p, compX, yText, bTop, visH, dispComp);
            p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
        }
    }

    // 光标: 位置**必须**由 xOfIndex 给出 (含图片宽度), 否则图片后会偏移
    if (focused && m_cursorOn && !m_readOnly) {
        int curLine = 0, curCol = 0;
        locateIndex(m_cursor, curLine, curCol);
        const LineLayout& L = m_lines[static_cast<size_t>(curLine)];

        int cx = m_marginLeft - m_scrollX + xOfIndex(curLine, curCol);
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
        // 按**视觉单元**走一步: 图片标记整体跨过, 不会停进标记中间
        if (shift)                 moveCursor(stepVisual(-1), true);
        else if (hasSelection())   moveCursor(selStart(), false);   // 先折叠到选区起点
        else                       moveCursor(stepVisual(-1), false);
        // 水平移动: 刷新记忆列
        refreshDesiredColumn();
        event->accept();
        return;

    case JZKey::Key_Right:
        if (shift)                 moveCursor(stepVisual(1), true);
        else if (hasSelection())   moveCursor(selEnd(), false);     // 先折叠到选区终点
        else                       moveCursor(stepVisual(1), false);
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
            // 退格删"前一个视觉单元": 图片标记必须整段删掉, 否则会留下半截标记
            const int to   = m_cursor;
            const int from = stepVisual(-1);
            if (to > from)
                replaceRangeWithUndo(from, to - from, JZString(), false);
        }
        event->accept();        // 收尾 (重排/光标/信号) 由撤销命令完成
        return;

    case JZKey::Key_Delete:
        if (m_readOnly) break;
        if (hasSelection()) {
            deleteSelection();
        } else if (m_cursor < m_text.size()) {
            // Delete 删"后一个视觉单元" (同理, 图片整段删)
            const int from = m_cursor;
            const int to   = stepVisual(1);
            if (to > from)
                replaceRangeWithUndo(from, to - from, JZString(), false);
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

// 计算 IME 需要的位置, 以**屏幕逻辑坐标**给出 (唯一的几何来源, 供下面两条路径复用):
//   lineRect — 当前输入行(组字起点所在行)的矩形, 对应 COMPOSITIONFORM(CFS_RECT);
//   candPos  — 候选窗左上角锚点, 对应 CANDIDATEFORM(CFS_CANDIDATEPOS)。
// 返回 false 表示当前无输入位置可报 (无行布局 / 无原生窗口)。
//
// 锚点取**组字起点**而非当前光标: 组字时光标会在组字串里移动, 若用光标位置,
// 候选条左边缘会跟着一路右移, 看起来就是"偏了"。
bool JZAbstractTextEdit::imeWindowRect(JZRect& lineRect, JZPoint& candPos) const
{
    if (m_lines.empty())
        return false;

    JZWindow* win = window();
    if (!win || !win->nativeHandle())
        return false;

    JZFont font(kFontPixelSize);
    const int visH = JZFontMetrics::visualHeight(font);
    const int pad  = (lineHeight() - visH) / 2;

    int line = 0, col = 0;
    locateIndex(selStart(), line, col);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    // 必须走 xOfIndex: 否则图片之后开始组字时, 候选条会偏到图片左侧
    const int startX = m_marginLeft - m_scrollX + xOfIndex(line, col);
    const int startY = contentTop() + L.y + pad;
    const int lineRightX = m_marginLeft - m_scrollX + xOfIndex(line, L.length);
    const int bottomY    = startY + visH;

    // mapToGlobal: 控件坐标 -> 屏幕逻辑坐标 (内部已做 ×scale / ClientToScreen /
    // ÷scale), 与 Qt 一致; IME 候选条按此锚点摆放。
    const JZPoint sTop   = mapToGlobal(JZPoint(startX, startY));
    const JZPoint sRight = mapToGlobal(JZPoint(lineRightX, startY));
    const JZPoint sBot   = mapToGlobal(JZPoint(startX, bottomY));

    // 组字窗锚点 (相对组字起点/字形顶部微调) + 行右端/行底
    lineRect = JZRect(sTop.x() + kImeCompOffsetX,
                      sTop.y() + kImeCompOffsetY,
                      std::max(1, sRight.x() - sTop.x()),
                      std::max(1, sBot.y() - sTop.y() + kImeCandOffsetY));
    // 候选窗左上角: 当前输入行正下方
    candPos = JZPoint(sBot.x() + kImeCandOffsetX, sBot.y() + kImeCandOffsetY);

    // —— 调试: 设置环境变量 JZ_IME_DEBUG=1 后, 把算出的候选框坐标与窗口屏幕矩形
    // 写到 %TEMP%/jz_ime.log。用于判断"坐标算错"还是"算对了但输入法不认"。——
    if (getenv("JZ_IME_DEBUG")) {
        RECT wr = { 0, 0, 0, 0 };
        if (win && win->nativeHandle())
            GetWindowRect((HWND)win->nativeHandle(), &wr);
        const char* td = getenv("TEMP");
        std::string path = std::string(td ? td : "c:/temp") + "/jz_ime.log";
        std::ofstream f(path, std::ios::app);
        f << "[imeWindowRect] win_screen=(" << wr.left << "," << wr.top << ","
          << wr.right << "," << wr.bottom << ") caret_logical=(" << sTop.x()
          << "," << sTop.y() << ") lineRect=(" << lineRect.left() << ","
          << lineRect.top() << "," << lineRect.right() << "," << lineRect.bottom()
          << ") candPos=(" << candPos.x() << "," << candPos.y() << ")\n";
    }
    return true;
}

// IMR_QUERYCHARPOSITION 应答: 回报"组字串第 charPos 个字符"的屏幕位置 (屏幕逻辑坐标)。
//
// TSF 输入法 (QQ 拼音等) 定位候选窗走的正是这条: 它们不用 IMM 的
// ImmSetCandidateWindow(CFS_CANDIDATEPOS), 而是发 WM_IME_REQUEST +
// IMR_QUERYCHARPOSITION 问窗口"这个字符在屏幕哪儿", 据此摆候选框 —— 不处理时
// 候选框就掉到控件的默认位置 (表现为"贴在控件底部、不在输入字符下面")。
//
// 组字串是**独立绘制**的 (不在 m_lines 文本流里), 所以第 charPos 个字符的 x =
// 组字锚点 x + 前 charPos 个组字码点的宽度之和 (测宽走与绘制相同的 JZFontMetrics)。
bool JZAbstractTextEdit::imeQueryCharPosition(int charPos, JZPoint& pt, int& outLineHeight,
                                              JZRect& docRect) const
{
    if (m_lines.empty())
        return false;

    JZWindow* win = window();
    if (!win || !win->nativeHandle())
        return false;

    JZFont font(kFontPixelSize);
    const int visH = JZFontMetrics::visualHeight(font);
    const int pad  = (lineHeight() - visH) / 2;

    // 组字锚点 (与 imeWindowRect 同一来源: 组字起点所在行的起点)
    int line = 0, col = 0;
    locateIndex(selStart(), line, col);
    const LineLayout& L = m_lines[static_cast<size_t>(line)];

    int x = m_marginLeft - m_scrollX + xOfIndex(line, col);
    const int n = std::min(static_cast<int>(m_comp.size()), std::max(0, charPos));
    for (int i = 0; i < n; ++i)
        x += JZFontMetrics::textWidth(m_comp.mid(i, 1).toStdString(), font);

    const int topY = contentTop() + L.y + pad;
    pt = mapToGlobal(JZPoint(x, topY));
    outLineHeight = visH;

    // 可编辑区 (屏幕逻辑坐标): 用控件客户区近似
    const JZPoint tl = mapToGlobal(JZPoint(0, 0));
    const JZPoint br = mapToGlobal(JZPoint(width(), height()));
    docRect = JZRect(tl.x(), tl.y(), std::max(1, br.x() - tl.x()),
                     std::max(1, br.y() - tl.y()));
    return true;
}

void JZAbstractTextEdit::updateIMEWindowPos()
{
    JZRect lineRect;
    JZPoint candPos;
    if (!imeWindowRect(lineRect, candPos))
        return;

    // ⚠ 判据是"能不能拿到 IMC", 不是"有没有 nativeHandle": 虚拟桌面上的窗口也有句柄
    //   (那是 JZVirtualWindow 对象指针), 但它背后没有真 HWND, ImmGetContext 照样拿不到
    //   输入上下文 —— 那时这一份位置也得送出去, 否则候选框不跟虚拟桌面里的输入行走。
    HWND hwnd = nativeHwndOf(this);
    HIMC hIMC = hwnd ? ImmGetContext(hwnd) : nullptr;
    if (!hIMC) {
        // 自己动不了 IMM == 跑在虚拟桌面上: 把位置交给所属窗口的**后端**, 由它转给真正
        // 拿着 IMC 的那扇宿主窗口 (见 JZVirtualWindowBackend::imeSetPosition)。
        if (JZWindow* win = window())
            win->setImeWindowPos(lineRect, candPos);
        return;
    }

    JZWindow* win = window();
    const float scale = (win && win->device()) ? win->device()->scale() : 1.0f;

    // 组字窗: 用 CFS_RECT 同时给 TSF 输入法提供"候选框定位矩形"。
    // —— 关键修复 (一) ——
    // 现代 TSF 输入法 (Win10/11 自带微软拼音、QQ 拼音、新版搜狗等) 会**忽略**
    // CFS_CANDIDATEPOS, 改以"组字窗矩形"作为候选框落点。若只给 CFS_POINT(单点、
    // 无面积), TSF 拿不到矩形, 候选框便退化到默认位置 —— 正好表现为"候选框贴在
    // 控件底部、不在输入字符下面"。此处把组字窗设为覆盖当前输入行的矩形,
    // TSF 就会把候选框摆在这一行正下方。
    COMPOSITIONFORM cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.dwStyle = CFS_RECT;
    cf.ptCurrentPos.x = toPhysical(lineRect.left(), scale);
    cf.ptCurrentPos.y = toPhysical(lineRect.top(), scale);
    cf.rcArea.left   = toPhysical(lineRect.left(), scale);
    cf.rcArea.top    = toPhysical(lineRect.top(), scale);
    cf.rcArea.right  = toPhysical(lineRect.right(), scale);
    cf.rcArea.bottom = toPhysical(lineRect.bottom(), scale);
    ImmSetCompositionWindow(hIMC, &cf);

    // 候选窗左上角: 老式 IMM 输入法兜底 (直接放到当前输入行正下方一行)。
    // TSF 输入法此处被忽略, 改用上面组字窗的矩形来定位 (或走 WM_IME_REQUEST,
    // 见 JZWindow 的 jzHandleImeRequest), 行为一致。
    // 注意: 多行输入框若当前行已填满到框底, 候选窗会自然落在框底那一行下方,
    // 这是预期行为, 不是偏到框外。
    CANDIDATEFORM cdf;
    ZeroMemory(&cdf, sizeof(cdf));
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos.x = toPhysical(candPos.x(), scale);
    cdf.ptCurrentPos.y = toPhysical(candPos.y(), scale);
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
