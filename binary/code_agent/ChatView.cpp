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

// 思考卡: 折叠后只剩一条标题条
static constexpr int kFoldH    = 26;   // 折叠条高度
// 工具合并卡: 调用参数与"输出"两段之间的间距与标签行高
static constexpr int kSecPad    = 10;
static constexpr int kResLabelH = 18;

// 画小三角(用当前画笔颜色)。down=true 朝下(展开态, 提示可收起), 否则朝右(折叠态, 提示可展开)。
static void drawTriangle(JZPainter& p, int cx, int cy, int r, bool down)
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

// ==================== 工具输出截断 ====================
// 工具结果可能很长, 显示时只保留前后各 keepHead/keepTail 行原始文本,
// 中间以省略标记代替; 原始 m.text 不变(历史/导出仍保留完整内容)。
static std::string truncateToolText(const std::string& text, int keepHead, int keepTail)
{
    std::vector<std::string> raw;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            raw.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    int total = (int)raw.size();
    if (total <= keepHead + keepTail)          // 中间无可省略行, 原样返回
        return text;

    std::string out;
    for (int i = 0; i < keepHead; ++i)
    {
        out += raw[i];
        out += '\n';
    }
    out += "... (省略 " + std::to_string(total - keepHead - keepTail) + " 行) ...\n";
    for (int i = total - keepTail; i < total; ++i)
    {
        out += raw[i];
        out += '\n';
    }
    return out;
}

// ==================== 单条消息布局 ====================
void ChatView::layoutMsg(Msg& m)
{
    JZFont font(14);
    int cardW = width() - 2 * kOuter - (kScrollBarW + 4) - 2 * kCardPad;
    if (cardW < 20)
        cardW = 20;

    m.lines.clear();
    m.resultLines.clear();
    int lh = JZFontMetrics::textHeight(font) + kLineGap;

    // 思考卡折叠后只剩一条标题条
    if (m.role == Thinking && m.collapsed)
    {
        m.cardH = kFoldH;
        return;
    }

    // 工具调用 + 输出合并卡: 上段是调用参数, 下段(标签"输出")是工具结果
    if (m.role == Tool && m.hasResult)
    {
        wrapText(truncateToolText(m.text, 3, 3), font, cardW, m.lines);
        wrapText(truncateToolText(m.resultText, 3, 3), font, cardW, m.resultLines);
        m.cardH = kHeaderH + (int)m.lines.size() * lh + kSecPad + kResLabelH
                + (int)m.resultLines.size() * lh + 2 * kCardPad;
        return;
    }

    const std::string& renderText =
        (m.role == ToolResult) ? truncateToolText(m.text, 3, 3) : m.text;
    wrapText(renderText, font, cardW, m.lines);
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

    // 等待状态卡：紧随最后一条消息之后，同样参与滚动总高
    m_waitH = 0;
    if (m_waitShow)
    {
        JZFont font(14);
        int cardW = width() - 2 * kOuter - (kScrollBarW + 4) - 2 * kCardPad;
        if (cardW < 20)
            cardW = 20;
        wrapText("等待回复中...", font, cardW, m_waitLines);
        int lh = JZFontMetrics::textHeight(font) + kLineGap;
        m_waitH = kHeaderH + (int)m_waitLines.size() * lh + 2 * kCardPad;
        total += kGap + m_waitH;
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

// ==================== 流式卡切换 ====================
// 关闭当前打开的流式卡: 若是思考卡则自动折叠(思考已结束)。返回是否折叠了思考卡。
bool ChatView::closeStream()
{
    if (m_streamIdx < 0 || m_streamIdx >= (int)m_msgs.size())
        return false;
    if (m_msgs[m_streamIdx].role == Thinking)
    {
        m_msgs[m_streamIdx].collapsed = true;   // 思考完毕 -> 折叠
        m_streamIdx = -1;
        return true;
    }
    m_streamIdx = -1;
    return false;
}

// 新开一张卡作为流式目标: 若现有流式卡角色相同则直接续用。
int ChatView::openStream(Role role)
{
    if (m_streamIdx >= 0 && m_streamIdx < (int)m_msgs.size()
        && m_msgs[m_streamIdx].role == role)
        return m_streamIdx;
    closeStream();
    Msg m;
    m.role = role;
    m.collapsible = (role == Thinking);
    m_msgs.push_back(std::move(m));
    m_streamIdx = (int)m_msgs.size() - 1;
    return m_streamIdx;
}

int ChatView::hitTestMsg(int contentY) const
{
    int top = kOuter;
    for (size_t i = 0; i < m_msgs.size(); i++)
    {
        if (contentY >= top && contentY < top + m_msgs[i].cardH)
            return (int)i;
        top += m_msgs[i].cardH + kGap;
    }
    return -1;
}

// ==================== 消息追加 ====================
void ChatView::appendUser(const std::string& text)
{
    closeStream();
    Msg m; m.role = User; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendAssistant(const std::string& text)
{
    appendAssistantDelta(text);
    endStream();
}

void ChatView::appendAssistantDelta(const std::string& text)
{
    int idx = openStream(Assistant);
    m_msgs[idx].text += text;
    afterChange();
}

void ChatView::appendThinking(const std::string& text)
{
    closeStream();
    Msg m; m.role = Thinking; m.text = text;
    m.collapsible = true;
    m.collapsed = true;               // 整条导入的思考已完毕 -> 默认折叠
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendThinkingDelta(const std::string& text)
{
    // 正文已经开始后的"思考增量"无意义(正常时序思考总在正文前), 直接忽略
    if (m_streamIdx >= 0 && m_streamIdx < (int)m_msgs.size()
        && m_msgs[m_streamIdx].role == Assistant)
        return;
    int idx = openStream(Thinking);
    m_msgs[idx].text += text;
    afterChange();
}

void ChatView::appendTool(const std::string& name, const std::string& text)
{
    closeStream();                    // 思考卡若还开着, 到这里说明思考已结束
    Msg m; m.role = Tool; m.name = name; m.text = text;
    m_msgs.push_back(std::move(m));
    m_pendingToolIdx.push_back((int)m_msgs.size() - 1);
    afterChange();
}

void ChatView::appendToolResult(const std::string& text)
{
    closeStream();
    if (!m_pendingToolIdx.empty())
    {
        // 把输出回填给最近一条还没配对的工具调用卡(合并显示)
        int idx = m_pendingToolIdx.front();
        m_pendingToolIdx.erase(m_pendingToolIdx.begin());
        m_msgs[idx].resultText = text;
        m_msgs[idx].hasResult = true;
    }
    else
    {
        // 没有待配对的调用(历史导入等场景), 退化成独立的工具结果卡
        Msg m; m.role = ToolResult; m.text = text;
        m_msgs.push_back(std::move(m));
    }
    afterChange();
}

void ChatView::appendError(const std::string& text)
{
    closeStream();
    Msg m; m.role = Error; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::appendInfo(const std::string& text)
{
    closeStream();
    Msg m; m.role = Info; m.text = text;
    m_msgs.push_back(std::move(m));
    afterChange();
}

void ChatView::endStream()
{
    closeStream();
}

void ChatView::clear()
{
    m_msgs.clear();
    m_streamIdx = -1;
    m_pendingToolIdx.clear();
    m_waitShow = false;
    m_waitLines.clear();
    m_waitH = 0;
    m_scrollY = 0;
    m_autoFollow = true;
    m_totalH = 2 * kOuter;
    update();
}

void ChatView::setWait(bool show)
{
    if (m_waitShow == show)
        return;
    m_waitShow = show;
    if (!m_waitShow)
        m_waitLines.clear();
    afterChange();
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

void ChatView::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
    {
        // 思考卡整卡可点: 折叠 <-> 展开
        int idx = hitTestMsg(event->localPos().y() + m_scrollY);
        if (idx >= 0 && idx < (int)m_msgs.size() && m_msgs[idx].collapsible)
        {
            m_msgs[idx].collapsed = !m_msgs[idx].collapsed;
            afterChange();
            return;
        }
    }
    JZWidget::mousePressEvent(event);
}

// ==================== 绘制 ====================
// 头部右端的小操作标签([展开]/[收起])
static void drawHeaderAction(JZPainter& p, const JZFont& font, const std::string& action,
                             const JZColor& color, int xRight, int yTop)
{
    int aw = JZFontMetrics::textWidth(action, font);
    p.setFont(font);
    p.setPen(JZPen(color, 1));
    p.drawText(xRight - aw, yTop, action.c_str());
}

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
        case Thinking:    bg = JZColor(0xF4,0xF1,0xFB); border = JZColor(0xE0,0xD9,0xF0); head = JZColor(0x6B,0x4F,0xA0); body = JZColor(0x44,0x3B,0x5E); headText = "思考过程"; break;
        case Tool:        bg = JZColor(0xFD,0xF6,0xE3); border = JZColor(0xF0,0xE2,0xB8); head = JZColor(0xB5,0x6B,0x00); body = JZColor(0x55,0x3A,0x00); headText = "工具: " + m.name; break;
        case ToolResult:  bg = JZColor(0xEE,0xF4,0xEE); border = JZColor(0xD8,0xE8,0xD8); head = JZColor(0x2E,0x7D,0x32); body = JZColor(0x33,0x44,0x33); headText = "工具结果"; break;
        case Error:       bg = JZColor(0xFB,0xE9,0xE9); border = JZColor(0xF0,0xC8,0xC8); head = JZColor(0xC0,0x2A,0x2A); body = JZColor(0xA0,0x20,0x20); headText = "错误"; break;
        case Info:        bg = JZColor(0xF2,0xF2,0xF2); border = JZColor(0xE4,0xE4,0xE4); head = JZColor(0x77,0x77,0x77); body = JZColor(0x55,0x55,0x55); headText = "信息"; break;
        }

        // 思考卡折叠态: 一条可点击的标题条
        if (m.role == Thinking && m.collapsed)
        {
            p.setBrush(JZBrush(bg));
            p.setPen(JZPen(border, 1));
            p.drawRoundRect(contentLeft, y, cardW, m.cardH, kFoldH / 2);

            p.setPen(JZPen(head, 1));
            int cy = y + m.cardH / 2;
            int ax = contentLeft + kCardPad + 6;
            drawTriangle(p, ax, cy, 4, false);          // ▶ 折叠, 点击展开

            p.setFont(headerFont);
            p.drawText(ax + 10, cy - JZFontMetrics::textHeight(headerFont) / 2,
                       headText.c_str());

            drawHeaderAction(p, headerFont, "[展开]", head,
                             contentRight - kCardPad,
                             cy - JZFontMetrics::textHeight(headerFont) / 2);
            y += m.cardH + kGap;
            continue;
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

        // 思考卡(展开态): 标题右端提示可收起
        if (m.role == Thinking)
            drawHeaderAction(p, headerFont, "[收起]", head,
                             contentRight - kCardPad,
                             y + (kHeaderH - JZFontMetrics::textHeight(headerFont)) / 2);

        if (m.role == Tool && m.hasResult)
        {
            // 工具合并卡: 上段 = 调用参数, 分隔线, 下段 = "输出" + 工具结果
            int x0 = contentLeft + kCardPad;
            int x1 = contentRight - kCardPad;
            int ty = y + kHeaderH;

            p.setFont(bodyFont);
            p.setPen(JZPen(body, 1));
            for (size_t li = 0; li < m.lines.size(); li++)
                p.drawText(x0, ty + li * lh, m.lines[li].c_str());
            ty += (int)m.lines.size() * lh;

            // 分隔线
            p.setPen(JZPen(border, 1));
            p.drawLine(x0, ty + 4, x1, ty + 4);
            ty += kSecPad;

            // "输出" 小标签
            p.setFont(headerFont);
            p.setPen(JZPen(JZColor(0x2E, 0x7D, 0x32), 1));
            p.drawText(x0, ty, "输出");
            ty += kResLabelH;

            // 结果正文
            p.setFont(bodyFont);
            p.setPen(JZPen(JZColor(0x33, 0x66, 0x3B), 1));
            for (size_t li = 0; li < m.resultLines.size(); li++)
                p.drawText(x0, ty + li * lh, m.resultLines[li].c_str());

            y += m.cardH + kGap;
            continue;
        }

        // 普通正文
        p.setFont(bodyFont);
        p.setPen(JZPen(body, 1));
        int ty = y + kHeaderH;
        for (size_t li = 0; li < m.lines.size(); li++)
            p.drawText(contentLeft + kCardPad, ty + li * lh, m.lines[li].c_str());

        y += m.cardH + kGap;
    }

    // "等待回复中..." 状态卡：画在最后一条消息之后，用醒目一点的浅黄色
    if (m_waitShow && m_waitH > 0 && y + m_waitH >= 0 && y <= h)
    {
        JZColor bg(0xFF, 0xF9, 0xE0), border(0xF2, 0xE3, 0xC2);
        JZColor head(0xA8, 0x6A, 0x00), body(0x8A, 0x6D, 0x3A);

        p.setBrush(JZBrush(bg));
        p.setPen(JZPen(border, 1));
        p.drawRoundRect(contentLeft, y, cardW, m_waitH, 8);

        p.setFont(headerFont);
        p.setPen(JZPen(head, 1));
        p.drawText(contentLeft + kCardPad, y + (kHeaderH - JZFontMetrics::textHeight(headerFont)) / 2,
                   "等待中");

        p.setFont(bodyFont);
        p.setPen(JZPen(body, 1));
        int ty = y + kHeaderH;
        for (size_t li = 0; li < m_waitLines.size(); li++)
            p.drawText(contentLeft + kCardPad, ty + li * lh, m_waitLines[li].c_str());
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
