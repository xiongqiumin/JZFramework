#include "JZComboBox.h"

JZ_OBJECT_IMPL(JZComboBox, JZWidget)

JZComboBox::JZComboBox(JZWidget* parent)
    : JZWidget(lv_dropdown_create, parent)
    , currentIndexChanged(this)
    , currentTextChanged(this)
{
}

void JZComboBox::addItem(const std::string& text)
{
    lv_dropdown_add_option(m_lv, text.c_str(), LV_DROPDOWN_POS_LAST);
}

void JZComboBox::addItems(const std::vector<std::string>& texts)
{
    for (const auto& t : texts)
        lv_dropdown_add_option(m_lv, t.c_str(), LV_DROPDOWN_POS_LAST);
}

void JZComboBox::insertItem(int index, const std::string& text)
{
    lv_dropdown_add_option(m_lv, text.c_str(), index);
}

void JZComboBox::removeItem(int index)
{
    
}

void JZComboBox::clear()
{
}

int JZComboBox::currentIndex() const
{
    return (int)lv_dropdown_get_selected(m_lv);
}

void JZComboBox::setCurrentIndex(int index)
{
    lv_dropdown_set_selected(m_lv, index);
}

std::string JZComboBox::currentText() const
{
    char buf[256] = {};
    lv_dropdown_get_selected_str(m_lv, buf, sizeof(buf));
    return buf;
}

void JZComboBox::setCurrentText(const std::string& text)
{
    const char* opts = lv_dropdown_get_options(m_lv);
    if (!opts) return;

    int idx = 0;
    const char* p = opts;
    while (*p) {
        const char* start = p;
        while (*p && *p != '\n') ++p;
        if (text.compare(0, std::string::npos, start, p - start) == 0) {
            setCurrentIndex(idx);
            return;
        }
        ++idx;
        if (*p == '\n') ++p;
    }
}

std::string JZComboBox::itemText(int index) const
{
    const char* opts = lv_dropdown_get_options(m_lv);
    if (!opts) return {};

    int i = 0;
    const char* p = opts;
    while (*p) {
        const char* start = p;
        while (*p && *p != '\n') ++p;
        if (i == index)
            return std::string(start, p - start);
        ++i;
        if (*p == '\n') ++p;
    }
    return {};
}

int JZComboBox::count() const
{
    return (int)lv_dropdown_get_option_count(m_lv);
}

void JZComboBox::onLvEvent(lv_event_t* e)
{
    JZWidget::onLvEvent(e);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        int idx = currentIndex();
        currentIndexChanged.emit(idx);
        currentTextChanged.emit(currentText());
    }
}
