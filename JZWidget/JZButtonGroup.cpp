#include "JZButtonGroup.h"
#include "JZWidget.h"

JZ_OBJECT_IMPL(JZButtonGroup, JZObject)

JZButtonGroup::JZButtonGroup(JZObject* parent)
    : JZObject(parent)
    , sigButtonClicked(this)
    , sigIdClicked(this)
    , sigButtonToggled(this)
    , sigIdToggled(this)
{
}

JZButtonGroup::~JZButtonGroup()
{
    // 按钮只借用: 断开反向指针, 免得活着的按钮留着指向已销毁组的悬垂
    // (见 JZWidget::m_buttonGroup / notifyButtonGroupChecked)。
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        JZObject* o = m_entries[i].button.object();
        if (!o)
            continue;
        JZWidget* w = static_cast<JZWidget*>(o);
        if (w->m_buttonGroup == this)
            w->m_buttonGroup = nullptr;
    }
}

bool JZButtonGroup::addButton(JZWidget* button, int id)
{
    if (!button || !button->isCheckable())
        return false;
    if (entryOf(button))
        return false;                     // 已在组内 (Qt: 重复添加被忽略)

    if (id == -1)
        id = m_nextAutoId--;

    Entry e;
    e.button = button->guardPointer();
    e.id = id;
    m_entries.push_back(e);

    button->m_buttonGroup = this;

    // 加入时按钮已选中: 直接登记为组的选中项 (与 Qt 一致, 不发信号)
    if (!m_checkedButton && button->isChecked())
        m_checkedButton = button->guardPointer();

    return true;
}

void JZButtonGroup::removeButton(JZWidget* button)
{
    if (!button)
        return;

    for (auto it = m_entries.begin(); it != m_entries.end(); ++it)
    {
        if (it->button.object() == button)
        {
            if (button->m_buttonGroup == this)
                button->m_buttonGroup = nullptr;
            m_entries.erase(it);
            break;
        }
    }

    if (m_checkedButton.object() == button)
        m_checkedButton = JZObjectPointer();
}

bool JZButtonGroup::contains(JZWidget* button) const
{
    if (!button)
        return false;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].button.object() == button)
            return true;
    }
    return false;
}

std::vector<JZWidget*> JZButtonGroup::buttons() const
{
    std::vector<JZWidget*> out;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        JZObject* o = m_entries[i].button.object();
        if (o)
            out.push_back(static_cast<JZWidget*>(o));
    }
    return out;
}

JZWidget* JZButtonGroup::button(int id) const
{
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        JZObject* o = m_entries[i].button.object();
        if (o && m_entries[i].id == id)
            return static_cast<JZWidget*>(o);
    }
    return nullptr;
}

int JZButtonGroup::id(JZWidget* button) const
{
    if (!button)
        return -1;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].button.object() == button)
            return m_entries[i].id;
    }
    return -1;
}

void JZButtonGroup::setId(JZWidget* button, int id)
{
    Entry* e = entryOf(button);
    if (e)
        e->id = id;
}

JZWidget* JZButtonGroup::checkedButton() const
{
    JZObject* o = m_checkedButton.object();
    return o ? static_cast<JZWidget*>(o) : nullptr;
}

int JZButtonGroup::checkedId() const
{
    JZWidget* b = checkedButton();
    return b ? id(b) : -1;
}

void JZButtonGroup::setExclusive(bool exclusive)
{
    m_exclusive = exclusive;
}

bool JZButtonGroup::isExclusive() const
{
    return m_exclusive;
}

JZButtonGroup::Entry* JZButtonGroup::entryOf(JZWidget* button)
{
    if (!button)
        return nullptr;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        if (m_entries[i].button.object() == button)
            return &m_entries[i];
    }
    return nullptr;
}

void JZButtonGroup::pruneDead()
{
    for (auto it = m_entries.begin(); it != m_entries.end(); )
    {
        if (!it->button.object())
            it = m_entries.erase(it);
        else
            ++it;
    }
}

// 由 JZWidget::notifyButtonGroupChecked 回调: 成员选中态变化 -> 维护互斥 + 转发信号
void JZButtonGroup::onButtonChecked(JZWidget* button)
{
    pruneDead();

    if (m_updating || !entryOf(button))
        return;

    const bool checked = button->isChecked();

    if (checked)
    {
        JZObject* prev = m_checkedButton.object();
        m_checkedButton = button->guardPointer();

        // 互斥: 取消上一个选中项。重入保护 —— prev->setChecked(false) 会再次回调本函数,
        // 那一次被 m_updating 挡掉 (否则会递归)。
        if (m_exclusive && prev && prev != button)
        {
            m_updating = true;
            static_cast<JZWidget*>(prev)->setChecked(false);
            m_updating = false;
        }
    }
    else if (m_checkedButton.object() == button)
    {
        // 当前选中项被取消 (只能是程序化改动, 互斥组内用户点击被控件自身拦住): 组内暂无选中项
        m_checkedButton = JZObjectPointer();
    }

    const int bid = id(button);
    sigButtonToggled.emit(button, checked);
    sigIdToggled.emit(bid, checked);
}

// 由 JZWidget::notifyButtonGroupClicked 回调: 成员被点击 -> 转发信号
void JZButtonGroup::onButtonClicked(JZWidget* button)
{
    pruneDead();

    if (!entryOf(button))
        return;

    sigButtonClicked.emit(button);
    sigIdClicked.emit(id(button));
}
