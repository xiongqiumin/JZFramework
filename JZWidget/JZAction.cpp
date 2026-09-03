#include "JZAction.h"
#include "JZMenu.h"

JZ_OBJECT_IMPL(JZAction, JZObject)

JZAction::JZAction(JZObject* parent)
    : JZObject(parent)
    , triggered(this)
{
}

JZAction::JZAction(const std::string& text, JZObject* parent)
    : JZAction(parent)
{
    m_text = text;
}

void JZAction::setText(const std::string& text)
{
    m_text = text;
}

std::string JZAction::text() const
{
    return m_text;
}

void JZAction::setIcon(const JZImage& icon)
{
    m_icon = icon;
}

const JZImage& JZAction::icon() const
{
    return m_icon;
}

void JZAction::setShortcut(const std::string& shortcut)
{
    m_shortcut = shortcut;
}

std::string JZAction::shortcut() const
{
    return m_shortcut;
}

void JZAction::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

bool JZAction::isEnabled() const
{
    return m_enabled;
}

void JZAction::setCheckable(bool checkable)
{
    m_checkable = checkable;
}

bool JZAction::isCheckable() const
{
    return m_checkable;
}

void JZAction::setChecked(bool checked)
{
    m_checked = checked;
}

bool JZAction::isChecked() const
{
    return m_checked;
}

void JZAction::setSeparator(bool separator)
{
    m_separator = separator;
}

bool JZAction::isSeparator() const
{
    return m_separator;
}

void JZAction::setSubMenu(JZMenu* menu)
{
    m_submenu = menu;
}

JZMenu* JZAction::subMenu() const
{
    return m_submenu;
}

void JZAction::trigger()
{
    if (m_separator || !m_enabled)
        return;
    if (m_checkable)
        m_checked = !m_checked;
    triggered.emit();
}
