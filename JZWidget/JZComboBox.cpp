#include "JZComboBox.h"
#include "JZWindow.h"
#include "JZListWidget.h"
#include "JZPainter.h"
#include "JZFontMetrics.h"
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

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
    JZFont font(14);
    p.setFont(font);
    if (m_current >= 0 && m_current < (int)m_items.size())
        p.drawText(4, JZFontMetrics::centerY(font, h), m_items[m_current].c_str());

    // 下拉箭头
    int ax = w - 16, ay = h / 2;
    p.setPen(JZPen(JZColor(0x55, 0x55, 0x55), 1));
    p.drawLine(ax, ay - 3, ax + 6, ay + 3);
    p.drawLine(ax + 6, ay + 3, ax + 12, ay - 3);
}

// ==================== 下拉弹层 ====================
// 弹层 = JZWindow(Popup) + JZListWidget: 点击项/Enter 收编并关闭,
// Esc/外部点击关闭 (Popup 抓取 + JZWindow::keyPressEvent)。
// 注意弹层开着时, 对 combo 自身的点击被弹层抓取吞掉, 不会"关闭又重开"。

void JZComboBox::ensurePopup()
{
    if (m_popup)
        return;

    m_popup = new JZWindow(this);
    m_popup->setWindowType(JZWindow::Popup);
    m_popupList = new JZListWidget(m_popup);

    // 激活 (点击项 / Enter): 收编选择并关闭弹层
    connect(&m_popupList->sigActivated, [this](int idx) {
        if (idx >= 0 && idx < (int)m_items.size() && idx != m_current) {
            setCurrentIndex(idx);
            update();
            currentIndexChanged.emit(m_current);
            currentTextChanged.emit(currentText());
        }
        hidePopup();
    });
}

bool JZComboBox::isPopupOpen() const
{
    return m_popup && m_popup->isVisible();
}

void JZComboBox::showPopup()
{
    if (isPopupOpen() || m_items.empty())
        return;
    ensurePopup();

    // 同步条目与当前选中 (允许 open 后 combo 条目有变化)
    m_popupList->clear();
    for (const auto& t : m_items)
        m_popupList->addItem(t);
    m_popupList->setCurrentIndex(std::max(0, m_current));

    // 尺寸: 宽同 combo, 高 = 可见项数 * 行高 + 边距 (超出 8 项滚屏)
    const int maxVisible = 8;
    int h = std::min((int)m_items.size(), maxVisible) * m_popupList->itemHeight() + 4;
    m_popup->resize(width(), h);

    // 位置: combo 左下角; 屏幕下方放不下则向上翻
    JZPoint below = mapToScreen(JZPoint(0, height()));
    JZPoint above = mapToScreen(JZPoint(0, 0)) - JZPoint(0, h);
    float s = (window() && window()->device()) ? window()->device()->scale() : 1.0f;
    int screenH = (int)(GetSystemMetrics(SM_CYSCREEN) / s);
    JZPoint pos = (below.y() + h <= screenH) ? below : above;

    m_popup->show();            // 惰性创建 + SetCapture
    m_popup->moveScreen(pos.x(), pos.y());
    m_popupList->setFocus();    // 键盘导航交给列表
    m_popup->execModal(false);  // 不禁用 owner; 外点由弹层抓取吞掉
}

void JZComboBox::hidePopup()
{
    if (isPopupOpen())
        m_popup->hide();        // -> execModal 退出
}

// ==================== 鼠标 / 滚轮 ====================

void JZComboBox::mousePressEvent(JZMouseEvent* event)
{
    if (event->button() == JZMouseEvent::LeftButton)
        showPopup();   // 弹层开着时点击会被其抓取吞掉并关闭, 不会进到这里
}

void JZComboBox::wheelEvent(JZMouseEvent* event)
{
    selectNext(event->wheelDelta() > 0 ? -1 : 1);
}

void JZComboBox::selectNext(int dir)
{
    int n = (int)m_items.size();
    if (n == 0)
        return;
    int idx = ((currentIndex() + dir) % n + n) % n;
    if (idx == m_current)
        return;
    setCurrentIndex(idx);
    update();
    currentIndexChanged.emit(m_current);
    currentTextChanged.emit(currentText());
}
