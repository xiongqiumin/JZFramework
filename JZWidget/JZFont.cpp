#include "JZFont.h"

JZFont::JZFont(int size)
    : m_size(size)
{
}

JZFont::JZFont(const std::string& family, int size, int style)
    : m_family(family), m_size(size), m_style(style)
{
}

void JZFont::setFamily(const std::string& family)
{
    m_family = family;
}

const std::string& JZFont::family() const
{
    return m_family;
}

void JZFont::setSize(int size)
{
    m_size = size;
}

int JZFont::size() const
{
    return m_size;
}

void JZFont::setStyle(int style)
{
    m_style = style;
}

int JZFont::style() const
{
    return m_style;
}

void JZFont::setBold(bool on)
{
    if (on) m_style |= Bold;        else m_style &= ~Bold;
}

void JZFont::setItalic(bool on)
{
    if (on) m_style |= Italic;      else m_style &= ~Italic;
}

void JZFont::setUnderline(bool on)
{
    if (on) m_style |= Underline;   else m_style &= ~Underline;
}

void JZFont::setStrikeOut(bool on)
{
    if (on) m_style |= StrikeOut;   else m_style &= ~StrikeOut;
}

bool JZFont::bold() const      { return (m_style & Bold) != 0; }
bool JZFont::italic() const    { return (m_style & Italic) != 0; }
bool JZFont::underline() const { return (m_style & Underline) != 0; }
bool JZFont::strikeOut() const { return (m_style & StrikeOut) != 0; }

bool JZFont::operator==(const JZFont& o) const
{
    return m_family == o.m_family && m_size == o.m_size && m_style == o.m_style;
}

bool JZFont::operator!=(const JZFont& o) const
{
    return !(*this == o);
}
