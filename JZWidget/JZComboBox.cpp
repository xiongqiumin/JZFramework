#include "JZComboBox.h"

JZ_OBJECT_IMPL(JZComboBox, JZWidget)

JZComboBox::JZComboBox(JZWidget* parent)
    : JZWidget(parent)
    , currentIndexChanged(this)
    , currentTextChanged(this)
{
}

void JZComboBox::addItem(const std::string& text)
{}

void JZComboBox::addItems(const std::vector<std::string>& texts)
{}

void JZComboBox::insertItem(int index, const std::string& text)
{}

void JZComboBox::removeItem(int index)
{
    
}

void JZComboBox::clear()
{
}

int JZComboBox::currentIndex() const
{ return 0; }

void JZComboBox::setCurrentIndex(int index)
{}

std::string JZComboBox::currentText() const
{ return {}; }

void JZComboBox::setCurrentText(const std::string& text)
{}

std::string JZComboBox::itemText(int index) const
{ return {}; }

int JZComboBox::count() const
{ return 0; }

void JZComboBox::onLvEvent(void* e)
{}
