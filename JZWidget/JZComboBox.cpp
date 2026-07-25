#include "JZComboBox.h"
#include "JZPainter.h"

JZ_OBJECT_IMPL(JZComboBox, JZWidget)

JZComboBox::JZComboBox(JZWidget* parent)
    : JZWidget(parent)
    , currentIndexChanged(this)
    , currentTextChanged(this)
{
}

void JZComboBox::addItem(const std::string& text)
{
    m_items.push_back(text);
    if (m_current < 0) m_current = 0;
}

void JZComboBox::addItems(const std::vector<std::string>& texts)
{
    for (auto& t : texts) m_items.push_back(t);
    if (m_current < 0 && !m_items.empty()) m_current = 0;
}

void JZComboBox::insertItem(int index, const std::string& text)
{
    if (index < 0 || index > (int)m_items.size()) index = (int)m_items.size();
    m_items.insert(m_items.begin() + index, text);
    if (m_current < 0) m_current = 0;
}

void JZComboBox::removeItem(int index)
{
    if (index < 0 || index >= (int)m_items.size()) return;
    m_items.erase(m_items.begin() + index);
    if (m_current >= (int)m_items.size()) m_current = (int)m_items.size() - 1;
}

void JZComboBox::clear()
{
    m_items.clear();
    m_current = -1;
}

int JZComboBox::currentIndex() const
{ return m_current; }

void JZComboBox::setCurrentIndex(int index)
{
    if (index >= 0 && index < (int)m_items.size())
        m_current = index;
}

std::string JZComboBox::currentText() const
{
    if (m_current >= 0 && m_current < (int)m_items.size())
        return m_items[m_current];
    return {};
}

void JZComboBox::setCurrentText(const std::string& text)
{
    for (int i = 0; i < (int)m_items.size(); ++i) {
        if (m_items[i] == text) { m_current = i; break; }
    }
}

std::string JZComboBox::itemText(int index) const
{
    if (index >= 0 && index < (int)m_items.size())
        return m_items[index];
    return {};
}

int JZComboBox::count() const
{ return (int)m_items.size(); }

JZSize JZComboBox::sizeHint() const
{
    int w = m_width > 0 ? m_width : 120;
    return JZSize(w, 28);
}

void JZComboBox::paintEvent(JZPainterEvent* event)
{
    JZPainter p(this);
    int w = width(), h = height();

    p.setBrush(JZBrush(JZColor(0xff, 0xff, 0xff)));
    p.setPen(JZPen(JZColor(0x80, 0x80, 0x80), 1));
    p.drawRoundRect(0, 0, w, h, 3);

    p.setPen(JZPen(JZColor(0x20, 0x20, 0x20), 1));
    p.setFont(JZFont(14));
    if (m_current >= 0 && m_current < (int)m_items.size()) {
        p.drawText(4, (h - 14) / 2, m_items[m_current].c_str());
    }

    // 下拉箭头
    int ax = w - 16, ay = h / 2;
    p.setPen(JZPen(JZColor(0x55, 0x55, 0x55), 1));
    p.drawLine(ax, ay - 3, ax + 6, ay + 3);
    p.drawLine(ax + 6, ay + 3, ax + 12, ay - 3);
}
