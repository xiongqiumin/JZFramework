#include "JZTextEdit.h"
#include "JZPainter.h"

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

    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.setFont(JZFont(14));

    std::string disp = m_password ? std::string(m_text.size(), '*') : m_text;

    // 多行: 按 '\n' 分行绘制 (单行文本只有一行, 逻辑相同)
    const int lineHeight = 18;
    const int textH = 14;
    size_t start = 0;
    int row = 0;
    while (2 + row * lineHeight + textH <= h) {
        size_t end = disp.find('\n', start);
        std::string line = (end == std::string::npos)
            ? disp.substr(start) : disp.substr(start, end - start);
        if (!line.empty())
            p.drawText(4, 2 + row * lineHeight, line.c_str());
        if (end == std::string::npos)
            break;
        start = end + 1;
        ++row;
    }
}
