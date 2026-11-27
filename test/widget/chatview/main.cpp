// 变高自绘列表 (JZListWidget + 自绘 item) 示例: 流式聊天气泡
//
// 分工:
//   - 框架(JZListWidget): 滚动、视口、滚动条、虚拟化、变高布局(前缀和 + 二分)、
//     "点击先给项自己判、没消费才选中"、"Ctrl+C 先给当前项" —— 完全不知道"聊天"。
//   - 本文件(ChatItem): 卡片长什么样、多高(heightForWidth)、点击折叠/拖选、复制选区。
//
// 用法: 输入框里打字回车(或点"发送"), 会模拟一段流式回复:
//   思考过程(流式续接, 收尾自动折叠) -> 工具调用(参数 + 稍后回填的输出) -> 正文逐字流式。
#include "JZApplication.h"
#include "JZWindow.h"
#include "JZWidget.h"
#include "JZListWidget.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZLayout.h"
#include "JZTimer.h"
#include "JZKeyEvent.h"
#include "JZMouseEvent.h"
#include "JZUtf8Iterator.h"
#include "JZImage.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZTextEmoji.h"
#include "JZEmoji.h"
#include "JZEmojiImageMap.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZPixmap.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // 剪贴板

namespace {

// ---- 卡片几何 ----
constexpr int kCardPad    = 10;   // 卡片内边距
constexpr int kHeaderH    = 22;   // 标题行高
constexpr int kLineGap    = 6;    // 正文行间距
constexpr int kSecPad     = 10;   // 正文与第二段之间的间距
constexpr int kResLabelH  = 18;   // 第二段小标签行高
constexpr int kFoldH      = 26;   // 折叠态: 只剩一条标题条
constexpr int kThumbGap   = 6;
constexpr int kThumbMaxW  = 220;
constexpr int kThumbMaxH  = 150;

void drawTriangle(JZPainter& p, int cx, int cy, int r, bool down)
{
    if (down)
    {
        p.drawLine(cx - r, cy - r + 1, cx + r, cy - r + 1);
        p.drawLine(cx + r, cy - r + 1, cx, cy + r);
        p.drawLine(cx, cy + r, cx - r, cy - r + 1);
    }
    else
    {
        p.drawLine(cx - r + 1, cy - r, cx + r, cy);
        p.drawLine(cx + r, cy, cx - r + 1, cy + r);
        p.drawLine(cx - r + 1, cy + r, cx - r + 1, cy - r);
    }
}

// ==================== emoji 感知的文本分段 ====================
// 测量(换行)与绘制(带选区高亮)必须同口径: emoji 码点(含 VS16/ZWJ/肤色修饰符)
// 整体算一个不可分割单元, 取图集位图显示; 其余连续文字合成文本段。
int xAtByte(const std::vector<JZTextEmojiRun>& runs, int byteOff, const JZFont& font)
{
    int x = 0;
    for (const auto& r : runs) {
        const int bs = r.byteStart;
        const int be = r.byteStart + static_cast<int>(r.text.size());
        if (byteOff <= bs)
            return x;
        if (byteOff >= be) {
            x += r.width;
            continue;
        }
        if (r.emoji)
            return x;
        const int prefix = byteOff - bs;
        int cpCount = 0;
        JZUtf8Iterator it(r.text);
        while (it.isValid() && it.byteIndex() < prefix) {
            ++cpCount;
            ++it;
        }
        return x + JZFontMetrics::textWidth(r.text, font, cpCount);
    }
    return x;
}

int offsetForX(const std::vector<JZTextEmojiRun>& runs, int x, const JZFont& font)
{
    int best = 0, bestDist = INT_MAX;
    for (const auto& r : runs) {
        std::vector<int> cands;
        if (r.emoji) {
            cands.push_back(r.byteStart);
            cands.push_back(r.byteStart + static_cast<int>(r.text.size()));
        } else {
            const int cp = JZUtf8Iterator::countCodepoints(r.text);
            cands.push_back(r.byteStart);
            int b = 0;
            JZUtf8Iterator it(r.text);
            while (it.isValid() && b < cp) {
                ++it;
                b = it.byteIndex();
                cands.push_back(r.byteStart + b);
            }
        }
        for (int off : cands) {
            const int d = std::abs(xAtByte(runs, off, font) - x);
            if (d < bestDist) { bestDist = d; best = off; }
        }
    }
    return best;
}

// 长文本按行截断: 保留前 head 行与后 tail 行, 中间省略 (只影响显示)
std::string truncateLines(const std::string& text, int maxLines)
{
    if (maxLines <= 0)
        return text;
    std::vector<std::string> raw;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            raw.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    const int total = (int)raw.size();
    if (total <= maxLines)
        return text;
    const int head = (maxLines + 1) / 2;
    const int tail = maxLines - head;
    std::string out;
    for (int i = 0; i < head; ++i) { out += raw[i]; out += '\n'; }
    out += "... (省略 " + std::to_string(total - maxLines) + " 行) ...\n";
    for (int i = total - tail; i < total; ++i) { out += raw[i]; out += '\n'; }
    return out;
}

std::string tailText(const std::string& text, int keepChars)
{
    if (keepChars <= 0 || (int)text.size() <= keepChars)
        return text;
    return "......" + text.substr(text.size() - keepChars);
}

// 贪婪按码点换行, emoji 整段参与测量 (与绘制同口径)
void wrapText(const std::string& text, const JZFont& font, int maxW,
              std::vector<std::string>& out)
{
    out.clear();
    if (maxW <= 0)
        maxW = 1;

    std::string line;
    int lineW = 0;
    size_t i = 0;
    const size_t n = text.size();
    auto pushLine = [&]() { out.push_back(line); line.clear(); lineW = 0; };

    while (i < n)
    {
        char32_t cp;
        int len;
        if (!JZUtf8Iterator::decode(text.data(), n, i, cp, len)) { i += 1; continue; }
        const std::string cpStr = text.substr(i, len);

        if (cp == '\n') { pushLine(); i += len; continue; }

        std::string piece = cpStr;
        int cpW = 0;
        if (JZEmoji::isEmoji(cp))
        {
            size_t j = i + len;
            while (j < n)
            {
                char32_t nc; int nl;
                if (!JZUtf8Iterator::decode(text.data(), n, j, nc, nl) || nl <= 0)
                    break;
                if (JZEmoji::isEmojiModifier(nc) || JZEmoji::isEmoji(nc)) {
                    piece += text.substr(j, nl);
                    j += nl;
                    continue;
                }
                break;
            }
            i = j;
            cpW = jzEmojiRunWidth(piece, font);
        }
        else
        {
            i += len;
            cpW = JZFontMetrics::textWidth(cpStr, font);
        }

        if (!line.empty() && lineW + cpW > maxW)
            pushLine();
        if (line.empty() && (cp == ' ' || cp == '\t'))
            continue;

        line += piece;
        lineW += cpW;
    }
    pushLine();
    if (out.empty())
        out.push_back("");
}

std::string utf8(char32_t cp)
{
    std::string out;
    JZUtf8Iterator::encode(cp, out);
    return out;
}

// ============================================================================
// ChatItem: 聊天卡片 (本示例里唯一的"具体绘制"代码)
// ============================================================================
class ChatItem : public JZListWidgetItem
{
public:
    enum Role { User = 0, Assistant, Thinking, Tool, ToolResult, Error, Info, Waiting, RoleCount };

    struct Style {
        std::string title;         // 卡片标题; 空 = 不画标题行
        std::string detailTitle;   // 第二段小标签
        JZColor bg, border, head, body;
        bool collapsible = false;
        int  visibleLines = 0;     // >0: 只显示首尾若干行
        int  tailChars = 0;        // >0: 只保留末尾若干字符
    };

    explicit ChatItem(Role role, const std::string& text = std::string())
        : m_role(role), m_text(text)
    {
    }

    static const Style& styleOf(Role role)
    {
        static Style s[RoleCount];
        static bool init = false;
        if (!init)
        {
            init = true;
            s[User].title = "你";
            s[User].bg = JZColor(0xE8, 0xF1, 0xFB); s[User].border = JZColor(0xC9, 0xDD, 0xF5);
            s[User].head = JZColor(0x1A, 0x6F, 0xC4); s[User].body = JZColor(0x22, 0x22, 0x22);

            s[Assistant].title = "助手";
            s[Assistant].bg = JZColor(0xF2, 0xF3, 0xF5); s[Assistant].border = JZColor(0xE2, 0xE4, 0xE8);
            s[Assistant].head = JZColor(0x2E, 0x7D, 0x4F); s[Assistant].body = JZColor(0x22, 0x22, 0x22);

            s[Thinking].title = "思考过程";
            s[Thinking].bg = JZColor(0xF4, 0xF1, 0xFB); s[Thinking].border = JZColor(0xE0, 0xD9, 0xF0);
            s[Thinking].head = JZColor(0x6B, 0x4F, 0xA0); s[Thinking].body = JZColor(0x44, 0x3B, 0x5E);
            s[Thinking].collapsible = true;     // 收尾由 Demo::endStream() 折叠
            s[Thinking].tailChars = 500;

            s[Tool].title = "工具";
            s[Tool].detailTitle = "输出";
            s[Tool].bg = JZColor(0xFD, 0xF6, 0xE3); s[Tool].border = JZColor(0xF0, 0xE2, 0xB8);
            s[Tool].head = JZColor(0xB5, 0x6B, 0x00); s[Tool].body = JZColor(0x55, 0x3A, 0x00);
            s[Tool].visibleLines = 6;

            s[ToolResult].title = "工具结果";
            s[ToolResult].bg = JZColor(0xEE, 0xF4, 0xEE); s[ToolResult].border = JZColor(0xD8, 0xE8, 0xD8);
            s[ToolResult].head = JZColor(0x2E, 0x7D, 0x32); s[ToolResult].body = JZColor(0x33, 0x44, 0x33);
            s[ToolResult].visibleLines = 6;

            s[Error].title = "错误";
            s[Error].bg = JZColor(0xFB, 0xE9, 0xE9); s[Error].border = JZColor(0xF0, 0xC8, 0xC8);
            s[Error].head = JZColor(0xC0, 0x2A, 0x2A); s[Error].body = JZColor(0xA0, 0x20, 0x20);

            s[Info].title = "信息";
            s[Info].bg = JZColor(0xF2, 0xF2, 0xF2); s[Info].border = JZColor(0xE4, 0xE4, 0xE4);
            s[Info].head = JZColor(0x77, 0x77, 0x77); s[Info].body = JZColor(0x55, 0x55, 0x55);

            s[Waiting].title = "等待中";
            s[Waiting].bg = JZColor(0xFF, 0xF9, 0xE0); s[Waiting].border = JZColor(0xF2, 0xE3, 0xC2);
            s[Waiting].head = JZColor(0xA8, 0x6A, 0x00); s[Waiting].body = JZColor(0x8A, 0x6D, 0x3A);
        }
        const int r = (role >= 0 && role < RoleCount) ? (int)role : (int)Assistant;
        return s[r];
    }

    Role role() const { return m_role; }
    bool collapsed() const { return m_collapsed; }

    // ---- 内容 (改完统一走 invalidate: 标记重排 + 通知列表只重测这一项) ----
    void appendText(const std::string& delta)
    {
        if (delta.empty()) return;
        m_text += delta;
        invalidate();
    }
    void setBody(const std::string& text)          { m_text = text; invalidate(); }
    void setDetail(const std::string& detail)      { m_detail = detail; m_hasDetail = true; invalidate(); }
    void setTitle(const std::string& title)        { m_title = title; invalidate(); }
    void setDetailTitle(const std::string& t)      { m_detailTitle = t; invalidate(); }
    void setImages(const std::vector<JZImage>& imgs) { buildThumbs(imgs); invalidate(); }

    void setCollapsed(bool c)
    {
        if (m_collapsed == c) return;
        m_collapsed = c;
        invalidate();
    }

    // 点击卡片标题条时切换折叠 (可折叠卡)
    void toggleCollapse() { setCollapsed(!m_collapsed); }

    // ---- JZListWidgetItem ----
    int heightForWidth(int width) override
    {
        layout(width);
        return m_cardH;
    }

    void paint(JZPainter& p, const JZRect& rect, const JZListItemPaintInfo& info) override
    {
        (void)info;
        layout(rect.width());

        const Style& st = styleOf(m_role);
        const std::string head = headText();
        const int hHead = head.empty() ? 0 : kHeaderH;
        const JZFont headerFont(12);
        const JZFont bodyFont(14);
        const int lh = JZFontMetrics::textHeight(bodyFont) + kLineGap;

        // 折叠态: 一条可点击的标题条
        if (m_collapsed)
        {
            p.setBrush(JZBrush(st.bg));
            p.setPen(JZPen(st.border, 1));
            p.drawRoundRect(rect.x(), rect.y(), rect.width(), rect.height(), kFoldH / 2);

            p.setPen(JZPen(st.head, 1));
            const int cy = rect.y() + rect.height() / 2;
            const int ax = rect.x() + kCardPad + 6;
            drawTriangle(p, ax, cy, 4, false);          // ▶ 折叠, 点击展开

            p.setFont(headerFont);
            p.drawText(ax + 10, cy - JZFontMetrics::textHeight(headerFont) / 2, head.c_str());
            drawHeaderAction(p, headerFont, "[展开]", st.head,
                             rect.x() + rect.width() - kCardPad,
                             cy - JZFontMetrics::textHeight(headerFont) / 2);
            return;
        }

        // 卡片
        p.setBrush(JZBrush(st.bg));
        p.setPen(JZPen(st.border, 1));
        p.drawRoundRect(rect.x(), rect.y(), rect.width(), rect.height(), 8);

        if (hHead > 0)
        {
            p.setFont(headerFont);
            p.setPen(JZPen(st.head, 1));
            p.drawText(rect.x() + kCardPad,
                       rect.y() + (kHeaderH - JZFontMetrics::textHeight(headerFont)) / 2,
                       head.c_str());
            if (st.collapsible)
                drawHeaderAction(p, headerFont, "[收起]", st.head,
                                 rect.x() + rect.width() - kCardPad,
                                 rect.y() + (kHeaderH - JZFontMetrics::textHeight(headerFont)) / 2);
        }

        const int x0 = rect.x() + kCardPad;
        int ty = rect.y() + hHead;

        if (!m_lines.empty())
        {
            p.setFont(bodyFont);
            p.setPen(JZPen(st.body, 1));
            drawLines(p, 0, x0, ty, lh);
            ty += (int)m_lines.size() * lh;
        }

        if (m_hasDetail)
        {
            p.setPen(JZPen(st.border, 1));
            p.drawLine(x0, ty + 4, rect.x() + rect.width() - kCardPad, ty + 4);
            ty += kSecPad;

            std::string label = m_detailTitle.empty() ? st.detailTitle : m_detailTitle;
            if (label.empty())
                label = "输出";
            p.setFont(headerFont);
            p.setPen(JZPen(st.head, 1));
            p.drawText(x0, ty, label.c_str());
            ty += kResLabelH;

            if (!m_detailLines.empty())
            {
                p.setFont(bodyFont);
                p.setPen(JZPen(st.body, 1));
                drawLines(p, 1, x0, ty, lh);
                ty += (int)m_detailLines.size() * lh;
            }
        }

        // 附图缩略图
        for (size_t i = 0; i < m_thumbs.size() && i < m_thumbRects.size(); i++)
        {
            const JZRect& tr = m_thumbRects[i];
            p.drawImage(m_thumbs[i], x0 + tr.x(), ty + tr.y(), tr.width(), tr.height());
        }
    }

    bool mousePress(const JZRect& rect, JZMouseEvent* e) override
    {
        if (e->button() != JZMouseEvent::LeftButton)
            return false;
        if (JZListWidget* w = widget())
        {
            w->setFocus();                 // 便于 Ctrl+C
            w->setCurrentItem(this, false); // 让列表把按键转给本项(keyPress), 不选中不高亮
        }

        layout(rect.width());
        const int lh = JZFontMetrics::textHeight(JZFont(14)) + kLineGap;
        const int head = headText().empty() ? 0 : kHeaderH;
        const int localY = e->localPos().y() - rect.y();

        // 折叠卡: 整卡点击展开; 展开卡: 点标题条收起
        if (m_collapsed) { toggleCollapse(); return true; }
        if (styleOf(m_role).collapsible && head > 0 && localY < head) { toggleCollapse(); return true; }

        // 命中正文 -> 起手拖选
        int part = 0;
        int first = head;
        if (m_hasDetail)
        {
            const int detailTop = head + (int)m_lines.size() * lh + kSecPad + kResLabelH;
            if (localY >= detailTop) { part = 1; first = detailTop; }
        }
        const std::vector<std::string>& lines = (part == 0) ? m_lines : m_detailLines;
        const int li = (localY - first) / lh;
        if (li < 0 || li >= (int)lines.size())
            return true;                        // 点在空白处: 消费掉, 不触发选中

        const JZFont bodyFont(14);
        const std::vector<JZTextEmojiRun> runs = jzSegmentTextEmoji(lines[li], bodyFont);
        const int off = offsetForX(runs, e->localPos().x() - (rect.x() + kCardPad), bodyFont);

        m_selPart = part;
        m_selLineA = m_selLineB = li;
        m_selOffA = m_selOffB = off;
        m_selecting = true;
        notifyChanged();
        return true;
    }

    bool mouseMove(const JZRect& rect, JZMouseEvent* e) override
    {
        if (!m_selecting)
            return false;
        layout(rect.width());
        const JZFont bodyFont(14);
        const int lh = JZFontMetrics::textHeight(bodyFont) + kLineGap;
        const int head = headText().empty() ? 0 : kHeaderH;
        const int first = (m_selPart == 1)
                        ? head + (int)m_lines.size() * lh + kSecPad + kResLabelH
                        : head;
        const std::vector<std::string>& lines = (m_selPart == 0) ? m_lines : m_detailLines;
        if (lines.empty())
            return true;

        int li = (e->localPos().y() - rect.y() - first) / lh;
        li = std::max(0, std::min(li, (int)lines.size() - 1));
        const std::vector<JZTextEmojiRun> runs = jzSegmentTextEmoji(lines[li], bodyFont);
        m_selLineB = li;
        m_selOffB = offsetForX(runs, e->localPos().x() - (rect.x() + kCardPad), bodyFont);
        notifyChanged();
        return true;
    }

    bool mouseRelease(const JZRect& rect, JZMouseEvent* e) override
    {
        (void)rect; (void)e;
        if (!m_selecting)
            return false;
        m_selecting = false;
        return true;
    }

    bool keyPress(JZKeyEvent* e) override
    {
        if (e->ctrl() && (e->key() == 'C' || e->key() == 'c'))
        {
            copySelection();
            return true;
        }
        return false;
    }

    std::string selectedText() const
    {
        if (m_selLineB < 0)
            return std::string();
        const std::vector<std::string>& lines = (m_selPart == 0) ? m_lines : m_detailLines;
        const int la = std::min(m_selLineA, m_selLineB);
        const int lb = std::max(m_selLineA, m_selLineB);
        if (la < 0 || lb >= (int)lines.size())
            return std::string();
        const int oa = (m_selLineA <= m_selLineB) ? m_selOffA : m_selOffB;
        const int ob = (m_selLineA <= m_selLineB) ? m_selOffB : m_selOffA;

        std::string out;
        for (int li = la; li <= lb; li++)
        {
            const std::string& ln = lines[li];
            int s = (li == la) ? oa : 0;
            int en = (li == lb) ? ob : (int)ln.size();
            s = std::max(0, std::min(s, (int)ln.size()));
            en = std::max(s, std::min(en, (int)ln.size()));
            if (li > la)
                out += '\n';
            out += ln.substr(s, en - s);
        }
        return out;
    }

    void copySelection()
    {
        const std::string s = selectedText();
        if (s.empty())
            return;
        if (!OpenClipboard(nullptr))
            return;
        EmptyClipboard();
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(wlen + 1) * sizeof(wchar_t));
        if (hMem) {
            wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hMem));
            if (dst) {
                MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), dst, wlen);
                dst[wlen] = 0;
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
    }

private:
    void invalidate()
    {
        m_needLayout = true;
        m_selecting = false;    // 内容变了, 旧选区(行号/偏移)已失效
        m_selLineB = -1;
        notifyChanged();        // 列表只重测这一项(前缀和是整数加法) + 重绘
    }

    std::string headText() const
    {
        const std::string& base = styleOf(m_role).title;
        if (m_title.empty()) return base;
        if (base.empty()) return m_title;
        return base + ": " + m_title;
    }

    // 按角色样式截断后的显示文本 (tailChars 优先, 再按 visibleLines 中间省略)
    std::string dispText(const std::string& text) const
    {
        const Style& st = styleOf(m_role);
        if (text.empty()) return text;
        std::string s = text;
        if (st.tailChars > 0) s = tailText(s, st.tailChars);
        if (st.visibleLines > 0) s = truncateLines(s, st.visibleLines);
        return s;
    }

    void buildThumbs(const std::vector<JZImage>& imgs)
    {
        m_thumbs.clear();
        for (const auto& img : imgs)
        {
            if (img.isNull())
                continue;
            m_thumbs.push_back(img.scaled(kThumbMaxW, kThumbMaxH,
                                          JZImage::AspectRatioMode::KeepAspectRatio,
                                          JZImage::TransformationMode::SmoothTransformation));
        }
    }

    // 换行 + 缩略图排布, 结果缓存在本项里 (宽度变了才重算)
    void layout(int width) const
    {
        const int w = std::max(20, width - 2 * kCardPad);
        if (!m_needLayout && w == m_layoutW)
            return;

        ChatItem* self = const_cast<ChatItem*>(this);
        self->m_layoutW = w;
        self->m_needLayout = false;

        const JZFont bodyFont(14);
        const int lh = JZFontMetrics::textHeight(bodyFont) + kLineGap;
        const int head = headText().empty() ? 0 : kHeaderH;

        if (m_collapsed)
        {
            self->m_lines.clear();
            self->m_detailLines.clear();
            self->m_cardH = kFoldH;
            return;
        }

        const std::string disp = dispText(m_text);
        self->m_lines.clear();
        if (!disp.empty())
            wrapText(disp, bodyFont, w, self->m_lines);

        self->m_detailLines.clear();
        int hDetail = 0;
        if (m_hasDetail)
        {
            const std::string dd = dispText(m_detail);
            if (!dd.empty())
                wrapText(dd, bodyFont, w, self->m_detailLines);
            hDetail = kSecPad + kResLabelH + (int)m_detailLines.size() * lh;
        }

        // 缩略图排布 (行内从左到右, 放不下折行)
        self->m_thumbRects.clear();
        int hThumb = 0;
        if (!m_thumbs.empty())
        {
            int x = 0, y = 0, rowH = 0;
            for (size_t i = 0; i < m_thumbs.size(); i++)
            {
                const int tw = m_thumbs[i].width();
                const int th = m_thumbs[i].height();
                if (tw <= 0 || th <= 0) { self->m_thumbRects.push_back(JZRect()); continue; }
                if (x > 0 && x + tw > w) { y += rowH + kThumbGap; x = 0; rowH = 0; }
                self->m_thumbRects.push_back(JZRect(x, y, tw, th));
                x += tw + kThumbGap;
                if (th > rowH) rowH = th;
            }
            if (y + rowH > 0)
                hThumb = y + rowH + kThumbGap;
        }

        self->m_cardH = head + (int)m_lines.size() * lh + hDetail + hThumb + 2 * kCardPad;
    }

    static void drawHeaderAction(JZPainter& p, const JZFont& font, const std::string& action,
                                 const JZColor& color, int xRight, int yTop)
    {
        const int aw = JZFontMetrics::textWidth(action, font);
        p.setFont(font);
        p.setPen(JZPen(color, 1));
        p.drawText(xRight - aw, yTop, action.c_str());
    }

    // 画一段正文(0=主正文, 1=第二段), 含选区高亮与 emoji 位图
    void drawLines(JZPainter& p, int part, int x0, int topY, int lh) const
    {
        const std::vector<std::string>& lines = (part == 0) ? m_lines : m_detailLines;
        if (lines.empty())
            return;

        const JZFont bodyFont(14);
        const int box = JZFontMetrics::textHeight(bodyFont);
        const bool sel = (m_selPart == part && m_selLineB >= 0);
        const int la = std::min(m_selLineA, m_selLineB);
        const int lb = std::max(m_selLineA, m_selLineB);
        const int oa = (m_selLineA <= m_selLineB) ? m_selOffA : m_selOffB;
        const int ob = (m_selLineA <= m_selLineB) ? m_selOffB : m_selOffA;

        for (size_t li = 0; li < lines.size(); li++)
        {
            const int yLine = topY + (int)li * lh;
            const std::vector<JZTextEmojiRun> runs = jzSegmentTextEmoji(lines[li], bodyFont);

            if (sel && (int)li >= la && (int)li <= lb)
            {
                const int sOff = ((int)li == la) ? oa : 0;
                const int eOff = ((int)li == lb) ? ob : (int)lines[li].size();
                if (eOff > sOff)
                {
                    const int xs = xAtByte(runs, sOff, bodyFont);
                    const int xe = xAtByte(runs, eOff, bodyFont);
                    p.fillRect(JZRect(x0 + xs, yLine, xe - xs, lh), JZColor(0xCF, 0xE4, 0xFF));
                }
            }

            int x = x0;
            for (const auto& r : runs)
            {
                if (r.emoji)
                {
                    const JZPixmap* pm = JZEmojiImageMap::instance().findPixmap(r.text, box);
                    if (pm && !pm->isNull())
                        p.drawPixmap(*pm, x, yLine, box, box);
                    else
                        p.drawText(x, yLine, r.text.c_str());
                }
                else
                {
                    p.drawText(x, yLine, r.text.c_str());
                }
                x += r.width;
            }
        }
    }

    Role m_role = Assistant;
    std::string m_title;         // 标题后缀 (工具名等)
    std::string m_text;          // 正文
    std::string m_detail;
    std::string m_detailTitle;
    bool m_hasDetail = false;
    std::vector<JZImage> m_thumbs;
    std::vector<JZRect> m_thumbRects;
    std::vector<std::string> m_lines;
    std::vector<std::string> m_detailLines;
    bool m_collapsed = false;

    mutable int  m_cardH = 0;
    mutable bool m_needLayout = true;
    mutable int  m_layoutW = -1;

    // 选区 (限定在本项某一段正文内: 行号 + 行内字节偏移)
    bool m_selecting = false;
    int  m_selPart = 0;
    int  m_selLineA = 0, m_selLineB = -1;
    int  m_selOffA = 0, m_selOffB = 0;
};

// ============================================================================
// 演示: 回车发送的输入行 + 假流式回复
// ============================================================================
class SendEdit : public JZLineEdit
{
public:
    explicit SendEdit(JZWidget* parent = nullptr) : JZLineEdit(parent) {}
    std::function<void()> onSend;

protected:
    void keyPressEvent(JZKeyEvent* event) override
    {
        if (event->key() == '\r' || event->key() == '\n')
        {
            if (onSend)
                onSend();
            event->accept();
            return;
        }
        JZLineEdit::keyPressEvent(event);
    }
};

std::vector<std::string> chunk(const std::string& text, size_t n)
{
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); i += n)
        out.push_back(text.substr(i, n));
    return out;
}

class Demo
{
public:
    JZListWidget* list = nullptr;
    SendEdit* edit = nullptr;
    JZTimer* timer = nullptr;
    ChatItem* stream = nullptr;      // 当前流式卡
    ChatItem* wait = nullptr;        // "等待中"卡
    std::vector<std::function<void()>> queue;

    // 同类续接, 换角色则另起一张(旧的思考卡收尾折叠)
    ChatItem* beginStream(ChatItem::Role role)
    {
        if (stream && stream->role() == role)
            return stream;
        endStream();
        ChatItem* it = new ChatItem(role);
        list->appendItem(it);
        stream = it;
        return it;
    }

    void endStream()
    {
        if (!stream)
            return;
        ChatItem* it = stream;
        stream = nullptr;
        if (it->role() == ChatItem::Thinking)
            it->setCollapsed(true);      // 过程日志收尾即折叠
    }

    // 清空: 先掐掉流式队列, 否则队列里握着的 item 指针会悬垂
    void reset()
    {
        if (timer)
            timer->stop();
        queue.clear();
        stream = nullptr;
        wait = nullptr;
        if (list)
            list->clear();
    }

    void showWait()
    {
        if (wait)
            return;
        wait = new ChatItem(ChatItem::Waiting, "等待回复中...");
        list->appendItem(wait);
    }

    void hideWait()
    {
        if (!wait)
            return;
        const int r = list->row(wait);
        if (r >= 0)
            delete list->takeItem(r);
        wait = nullptr;
    }

    void send()
    {
        const std::string prompt = edit->text();
        if (prompt.empty() || !queue.empty())
            return;                       // 上一条还在流式中
        edit->setText("");

        list->appendItem(new ChatItem(ChatItem::User, prompt));
        showWait();
        queue.clear();

        // 1) 思考过程: 增量续接同一张卡
        for (const auto& c : chunk("先看看用户问的是什么, 再决定要不要调用工具检索一下现有控件的用法。", 7))
        {
            const std::string part = c;
            queue.push_back([this, part] { beginStream(ChatItem::Thinking)->appendText(part); });
        }

        // 2) 工具调用: 参数先出, 输出稍后回填到同一张卡
        auto acc = std::make_shared<std::string>();
        auto tool = std::make_shared<ChatItem*>(nullptr);
        queue.push_back([this, tool] {
            ChatItem* it = new ChatItem(ChatItem::Tool, "{\"q\": \"JZListWidget 变高自绘\"}");
            it->setTitle("search");              // => 标题显示 "工具: search"
            it->setDetailTitle("输出");
            list->appendItem(it);
            *tool = it;
            endStream();
        });
        for (const auto& c : chunk("命中 3 条: JZListWidget.h / JZListWidget.cpp / 本示例。"
                                   "要点: heightForWidth() 报高度, paint() 画自己, "
                                   "改了内容调 notifyChanged() 只重测这一项。", 9))
        {
            const std::string part = c;
            queue.push_back([acc, tool, part] {
                *acc += part;
                if (*tool)
                    (*tool)->setDetail(*acc);
            });
        }

        // 3) 正文逐字流式 (带一个 emoji 走图集)
        const std::string reply =
            "在 JZListWidget 上做变高自绘项只要四步：\n"
            "  1. list->setDynamicItemHeight(true) 打开逐项测量；\n"
            "  2. 自定义 item 重写 heightForWidth(w) 报本行高度；\n"
            "  3. 重写 paint() 画自己，rect 已经扣掉内边距；\n"
            "  4. 改完内容调 notifyChanged()，列表只重测这一项。" + utf8(0x1F44D);
        for (const auto& c : chunk(reply, 5))
        {
            const std::string part = c;
            queue.push_back([this, part] { beginStream(ChatItem::Assistant)->appendText(part); });
        }

        // 4) 收尾
        queue.push_back([this] {
            endStream();
            hideWait();
        });

        timer->start(40);
    }

    void tick()
    {
        if (queue.empty())
        {
            timer->stop();
            return;
        }
        for (int k = 0; k < 2 && !queue.empty(); k++)
        {
            auto fn = queue.front();
            queue.erase(queue.begin());
            fn();
        }
    }
};

} // namespace

int app_main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    JZApplication app;

    JZWindow window;
    window.resize(760, 620);
    window.setTitle("JZListWidget Demo (变高自绘项 / 流式聊天)");

    Demo* demo = new Demo();            // 与窗口同生命周期

    JZVBoxLayout* vbox = new JZVBoxLayout(10, 10);

    JZListWidget* list = new JZListWidget(&window);
    list->setDynamicItemHeight(true);                  // 打开逐项测量
    list->setContentPadding(12, 12, 12, 12);           // 内容四周留白
    list->setSpacing(12);                              // 卡片间距
    list->setStickToBottom(true);                      // 贴底时新内容自动跟随
    list->setBackgroundColor(JZColor(0xF7, 0xF7, 0xF9));
    list->setEmptyText("在下面输入点什么, 回车试试流式回复");
    vbox->addWidget(list, 1);
    demo->list = list;

    // 底部输入条
    JZWidget* bar = new JZWidget(&window);
    JZHBoxLayout* hbox = new JZHBoxLayout(0, 8);
    SendEdit* edit = new SendEdit(bar);
    edit->setPlaceholderText("输入消息, 回车发送 (支持中文输入法)");
    hbox->addWidget(edit, 1);
    JZPushButton* btnSend = new JZPushButton(bar);
    btnSend->setText("发送");
    hbox->addWidget(btnSend, 0);
    JZPushButton* btnClear = new JZPushButton(bar);
    btnClear->setText("清空");
    hbox->addWidget(btnClear, 0);
    bar->setLayout(hbox);
    bar->setFixedHeight(40);
    vbox->addWidget(bar, 0);

    window.setLayout(vbox);

    demo->edit = edit;
    connect(&btnSend->sigClicked, [demo] { demo->send(); });
    edit->onSend = [demo] { demo->send(); };
    connect(&btnClear->sigClicked, [demo] { demo->reset(); });

    demo->timer = new JZTimer(&window);
    connect(&demo->timer->sigTimeout, [demo] { demo->tick(); });

    // 开场先给几条样例, 说明卡片长什么样
    list->appendItem(new ChatItem(ChatItem::User, "这是用户消息的气泡卡片"));
    {
        ChatItem* it = new ChatItem(ChatItem::Assistant,
            "这是助手消息。高度由项自己算(heightForWidth), 绘制由项自己画(paint), "
            "支持自动换行、emoji " + utf8(0x1F600) + "、长历史虚拟化, "
            "以及鼠标拖选后 Ctrl+C 复制。");
        list->appendItem(it);
    }
    list->appendItem(new ChatItem(ChatItem::Info, "点 \"发送\" 看思考过程/工具调用/正文三种流式卡片。"));
    list->scrollToBottom();

    window.setFocusWidget(edit);
    window.show();

    // 自动化/截图用: JZ_CHATVIEW_DEMO=1 时开场自动跑一轮
    const char* env = std::getenv("JZ_CHATVIEW_DEMO");
    if (env && std::string(env) == "1")
    {
        JZTimer::singleShot(200, [demo, edit] {
            edit->setText("JZListWidget 怎么自绘变高的项?");
            demo->send();
        });
    }

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
