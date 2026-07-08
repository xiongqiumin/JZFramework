#include "JZFont.h"

JZFont::JZFont(int size)
    : m_size(size)
{
}

void JZFont::setSize(int size)
{
    m_size = size;
}

int JZFont::size() const
{
    return m_size;
}
