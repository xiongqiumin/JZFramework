#include "ChatView.h"

#include "JZPainter.h"
#include "JZFontMetrics.h"
#include "JZUtf8Iterator.h"

#include <algorithm>

// 布局常量
static constexpr int kOuter      = 12;   // 控件四边外边距
static constexpr int kScrollBarW = 8;    // 滚动条宽度
static constexpr int kCardPad    = 10;   // 卡片内边距
static constexpr int kGap        = 12;   // 卡片间距
static constexpr int kHeaderH    = 22;   // 角色标题行高
static constexpr int kLineGap    = 6;    // 正文行间距
static constexpr int kWheelStep  = 40;   // 每格滚轮滚动像素

JZ_OBJECT_IMPL(ChatView, JZWidget)

ChatView::ChatView(JZWidget* parent)
    : JZWidget(parent)
{
}

// ==================== 文本换行 ====================
// 贪婪按码点换行: 遇到 '\n' 硬换行; 行宽超过 maxW 且已有内容则断行,
// 并丢弃新行开头的空白; 单个超宽码点绝不死循环 (已消费则允许越界)。
void ChatView::wrapText(const std::string& text, const JZFont& font,
                        int maxW, std::vector<std::string>& out)
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
        if (!JZUtf8Iterator::decode(text.data(), n, i, cp, len))
        {
            i += 1;                       // 非法字节: 跳过
            continue;
        }
        std::string cpStr = text.substr(i, len);
        i += len;

        if (cp == '\n')
        {
            pushLine();
            continue;
        }

        int cpW = JZFontMetrics::textWidth(cpStr, font);
        if (!line.empty() && lineW + cpW > maxW)
            pushLine();

        // 新行开头的空白直接丢弃 (避免顶部留白)
        if (line.empty() && (cp == ' ' || cp == '\t'))
            continue;

        line += cpStr;
        lineW += cpW;
    }
    pushLine();
    if (out.empty())
        out.push_back("");
}

// ==================== 单条消息布局 ====================
void ChatView::layoutMsg(Msg& m)
{
    JZFont font(14);
    int cardW = width() - 2 * kOuter - (kScrollBarW + 4) - 2 * kCardPad;
    if (cardW < 20)
        cardW = 20;
    wrapText(m.text, font, cardW, m.lines);

    int lh = JZFontMetrics::textHeight(font) + kLineGap;
    m.cardH = kHeaderH + (int)m.lines.size() * lh + 2 * kCardPad;
}

void ChatView::relayout()
{
    int total = kOuter;                   // 顶部外边距
    for (size_t i = 0; i < m_msgs.size(); i++)
    {
        layoutMsg(m_msgs[i]);
        total += m_msgs[i].cardH;
        if (i + 1 < m_msgs.size())
            total += kGap;
    }
    total += kOuter;                      // 底部外边距
    m_totalH = total;
}

int ChatView::contentHeight() const
{
    return m_totalH;
}

// ==================== 滚动管理 ====================
void ChatView::updateScroll(bool follow)
{
    int maxScroll = contentHeight() - height();
    if (maxScroll < 0)
        maxScroll = 0;

    if (follow)
        m_scrollY = maxScroll;
    if (m_scrollY < 0)
        m_scrollY = 0;
    if (m_scrollY > maxScroll)
        m_scrollY = maxScroll;

    // 跟随状态由"是否贴底"推导: 用户上滚即取消跟随, 回到底部恢复
    m_autoFollow = (m_scrollY >= maxScroll - 1);
}

void ChatView::afterChange()
{
    relayout();
    updateScroll(m_autoFollow);
    update();
}

void ChatView::scrollToBottom()
{
    m_autoFollow = true;
    afterChange();
}

// ==================== 消息追加 ====================
void ChatView::appendUser(const std::string& text)
{
    m_streamMsg = nullptr;
    Msg m; m.role = User; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendAssistantDelta(const std::string& text)
{
    if (!(m_streamMsg && m_streamMsg->role == Assistant))
    {
        m_streamMsg = nullptr;
        Msg m; m.role = Assistant; m.text.clear();
        m_msgs.push_back(std::move(m));
        m_streamMsg = &m_msgs.back();
    }
    m_streamMsg->text += text;
    afterChange();
}

void ChatView::appendTool(const std::string& name, const std::string& text)
{
    m_streamMsg = nullptr;
    Msg m; m.role = Tool; m.name = name; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendToolResult(const std::string& text)
{
    m_streamMsg = nullptr;
    Msg m; m.role = ToolResult; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendError(const std::string& text)
{
    m_streamMsg = nullptr;
    Msg m; m.role = Error; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendInfo(const std::string& text)
{
    m_streamMsg = nullptr;
    Msg m; m.role = Info; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::endStream()
{
    m_streamMsg = nullptr;
}

void ChatView::clear()
{
    m_msgs.clear();
    m_streamMsg = nullptr;
    m_scrollY = 0;
    m_autoFollow = true;
    m_totalH = 2 * kOuter;
    update();
}

// ==================== 事件 ====================
void ChatView::resizeEvent(JZResizeEvent* event)
{
    JZWidget::resizeEvent(event);
    relayout();
    updateScroll(m_autoFollow);
    update();
}

void ChatView::wheelEvent(JZMouseEvent* event)
{
    int d = event->wheelDelta();          // 上滚为正
    m_scrollY -= d * kWheelStep;          // 上滚 -> 看更早内容 -> 减小 scrollY
    int maxScroll = contentHeight() - height();
    if (maxScroll < 0)
        maxScroll = 0;
    m_scrollY = std::max(0, std::min(m_scrollY, maxScroll));
    m_autoFollow = (m_scrollY >= maxScroll - 1);
    update();
}

// ==================== 绘制 ====================
void ChatView::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setClipLocal(JZRect(0, 0, w, h));

    // 背景
    p.fillRect(0, 0, w, h, JZColor(0xF7, 0xF7, 0xF9));

    if (m_msgs.empty())
    {
        JZFont font(14);
        JZColor hint(0x99, 0x99, 0x99);
        p.setPen(JZPen(hint, 1));
        p.setFont(font);
        std::string s = "等待任务……";
        int tx = JZFontMetrics::centerX(s, font, w);
        p.drawText(tx, JZFontMetrics::centerY(font, h), s.c_str());
        return;
    }

    int contentLeft = kOuter;
    int contentRight = w - kOuter - (kScrollBarW + 4);
    int cardW = contentRight - contentLeft;

    JZFont headerFont(12);
    JZFont bodyFont(14);
    int lh = JZFontMetrics::textHeight(bodyFont) + kLineGap;

    int y = kOuter - m_scrollY;
    for (const auto& m : m_msgs)
    {
        if (y + m.cardH < 0 || y > h)     // 完全在视口外, 跳过
        {
            y += m.cardH + kGap;
            continue;
        }

        JZColor bg, border, head, body;
        std::string headText;
        switch (m.role)
        {
        case User:        bg = JZColor(0xE8,0xF1,0xFB); border = JZColor(0xC9,0xDD,0xF5); head = JZColor(0x1A,0x6F,0xC4); body = JZColor(0x22,0x22,0x22); headText = "你"; break;
        case Assistant:   bg = JZColor(0xF2,0xF3,0xF5); border = JZColor(0xE2,0xE4,0xE8); head = JZColor(0x2E,0x7D,0x4F); body = JZColor(0x22,0x22,0x22); headText = "助手"; break;
        case Tool:        bg = JZColor(0xFD,0xF6,0xE3); border = JZColor(0xF0,0xE2,0xB8); head = JZColor(0xB5,0x6B,0x00); body = JZColor(0x55,0x3A,0x00); headText = "工具: " + m.name; break;
        case ToolResult:  bg = JZColor(0xEE,0xF4,0xEE); border = JZColor(0xD8,0xE8,0xD8); head = JZColor(0x2E,0x7D,0x32); body = JZColor(0x33,0x44,0x33); headText = "工具结果"; break;
        case Error:       bg = JZColor(0xFB,0xE9,0xE9); border = JZColor(0xF0,0xC8,0xC8); head = JZColor(0xC0,0x2A,0x2A); body = JZColor(0xA0,0x20,0x20); headText = "错误"; break;
        case Info:        bg = JZColor(0xF2,0xF2,0xF2); border = JZColor(0xE4,0xE4,0xE4); head = JZColor(0x77,0x77,0x77); body = JZColor(0x55,0x55,0x55); headText = "信息"; break;
        }

        // 卡片
        p.setBrush(JZBrush(bg));
        p.setPen(JZPen(border, 1));
        p.drawRoundRect(contentLeft, y, cardW, m.cardH, 8);

        // 角色标题
        p.setFont(headerFont);
        p.setPen(JZPen(head, 1));
        p.drawText(contentLeft + kCardPad, y + (kHeaderH - JZFontMetrics::textHeight(headerFont)) / 2,
                   headText.c_str());

        // 正文
        p.setFont(bodyFont);
        p.setPen(JZPen(body, 1));
        int ty = y + kHeaderH;
        for (size_t li = 0; li < m.lines.size(); li++)
            p.drawText(contentLeft + kCardPad, ty + li * lh, m.lines[li].c_str());

        y += m.cardH + kGap;
    }

    // 滚动条 (内容高于视口时)
    int ch = contentHeight();
    if (ch > h)
    {
        int maxScroll = ch - h;
        int trackTop = kOuter;
        int trackH = h - 2 * kOuter;
        int thumbH = std::max(20, trackH * h / ch);
        int thumbY = trackTop + (trackH - thumbH) * m_scrollY / maxScroll;
        int sbX = w - kOuter - kScrollBarW;

        JZColor sb(0xC4, 0xC4, 0xCC);
        p.setBrush(JZBrush(sb));
        p.setPen(JZPen(sb, 1));
        p.drawRoundRect(sbX, thumbY, kScrollBarW, thumbH, kScrollBarW / 2);
    }
}
