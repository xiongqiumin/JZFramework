#include "JZTextEdit.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

#include <vector>

JZ_OBJECT_IMPL(JZTextEdit, JZWidget)

JZTextEdit::JZTextEdit(JZWidget* parent)
    : JZWidget(parent)
{
}

void JZTextEdit::setText(const std::string& text)
{
    m_text = text;
}

std::string JZTextEdit::text() const
{
    return m_text;
}

void JZTextEdit::setPassword(bool pw)
{
    m_password = pw;
}

JZSize JZTextEdit::sizeHint() const
{
    int w = m_width > 0 ? m_width : 160;
    int h = m_height > 0 ? m_height : 70;
    return JZSize(w, h);
}

void JZTextEdit::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x80, 0x80, 0x80), 1));
    p.drawRoundRect(0, 0, w, h, 3);

    if (m_text.empty())
        return;

    JZFont font(14);
    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.setFont(font);
    int lineH = JZFontMetrics::textHeight(font);

    std::string disp = m_password ? std::string(m_text.size(), '*') : m_text;

    // 按 '\n' 分行
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t end = disp.find('\n', start);
        lines.push_back((end == std::string::npos)
            ? disp.substr(start) : disp.substr(start, end - start));
        if (end == std::string::npos)
            break;
        start = end + 1;
    }

    // 单行垂直居中, 多行顶部对齐
    int step = lineH + 2;
    int y0 = (lines.size() == 1) ? (h - lineH) / 2 : 2;
    for (size_t r = 0; r < lines.size(); ++r) {
        int y = y0 + (int)r * step;
        if (y + lineH > h)
            break;
        if (!lines[r].empty())
            p.drawText(4, y, lines[r].c_str());
    }
}
