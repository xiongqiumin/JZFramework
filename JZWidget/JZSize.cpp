#include "JZSize.h"
#include "JZDebug.h"

JZSize::JZSize()
    : m_w(0), m_h(0)
{
}

JZSize::JZSize(int w, int h)
    : m_w(w), m_h(h)
{
}

int JZSize::width() const     { return m_w; }
int JZSize::height() const    { return m_h; }
void JZSize::setWidth(int w)  { m_w = w; }
void JZSize::setHeight(int h) { m_h = h; }

bool JZSize::isEmpty() const  { return m_w <= 0 || m_h <= 0; }
bool JZSize::isValid() const  { return m_w >= 0 && m_h >= 0; }

JZDebug operator<<(JZDebug d, const JZSize& sz)
{
    d.append(str_asprintf("{%d,%d}", sz.width(), sz.height()).c_str());
    return d;
}