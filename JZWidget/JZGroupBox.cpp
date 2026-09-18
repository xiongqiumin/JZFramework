#include "JZGroupBox.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"

#include <algorithm>

JZ_OBJECT_IMPL(JZGroupBox, JZWidget)

JZGroupBox::JZGroupBox(JZWidget* parent)
    : JZWidget(parent)
{
    m_content = new JZWidget(this);   // 内部内容容器 (透明, 露出父背景)
}

JZGroupBox::JZGroupBox(const std::string& title, JZWidget* parent)
    : JZGroupBox(parent)
{
    m_title = title;
}

JZSize JZGroupBox::sizeHint() const
{
    if (m_width > 0 && m_height > 0)
        return JZSize(m_width, m_height);
    JZSize cs = m_content ? m_content->sizeHint() : JZSize(0, 0);
    return JZSize(cs.width() + m_pad * 2, cs.height() + m_titleH + m_pad);
}

void JZGroupBox::setTitle(const std::string& t)
{
    m_title = t;
    update();
}

std::string JZGroupBox::title() const { return m_title; }

void JZGroupBox::setLayout(JZLayout* layout)
{
    if (m_content)
        m_content->setLayout(layout);
}

void JZGroupBox::setLayoutFromJs(JZLayout* layout)
{
    if (m_content)
        m_content->setLayoutFromJs(layout);   // 内容区那层同样"只借用不 delete"(布局归 JS)
}

JZLayout* JZGroupBox::appliedLayout() const
{
    return m_content ? m_content->layout() : nullptr;
}

void JZGroupBox::clearLayout()
{
    if (m_content)
        m_content->clearLayout();
}

void JZGroupBox::resize(int w, int h)
{
    JZWidget::resize(w, h);   // 基类设尺寸 + 重绘 (GroupBox 自身无布局)
    if (m_content)
        m_content->setGeometry(m_pad, m_titleH,
                               std::max(0, w - m_pad * 2),
                               std::max(0, h - m_titleH - m_pad));
}

void JZGroupBox::paintEvent(JZPainterEvent*)
{
    JZPainter p(this);
    int w = width(), h = height();
    int midY = m_titleH / 2;
    JZColor line(0xC0, 0xC4, 0xCC);
    p.setPen(JZPen(line, 1));

    // 左右竖线 + 底线
    p.drawLine(0, midY, 0, h - 1);
    p.drawLine(w - 1, midY, w - 1, h - 1);
    p.drawLine(0, h - 1, w - 1, h - 1);

    // 顶线: 标题处留缺口 (形成分组框经典的"缺口"效果)
    int tw = 0;
    if (!m_title.empty()) {
        JZFont font(13);
        p.setFont(font);
        tw = JZFontMetrics::textWidth(m_title, font);
    }
    int tx = m_pad;
    int gapL = tx - 3, gapR = tx + tw + 3;
    if (gapL > 0)        p.drawLine(0, midY, gapL, midY);
    if (gapR < w - 1)    p.drawLine(gapR, midY, w - 1, midY);

    // 标题文字 (缺口内)
    if (!m_title.empty()) {
        JZFont font(13);
        p.setFont(font);
        p.setPen(JZPen(JZColor(0x33, 0x33, 0x33), 1));
        p.drawText(tx, JZFontMetrics::centerY(font, m_titleH), m_title.c_str());
    }
}
