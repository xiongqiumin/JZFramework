#ifndef JZ_SIZE_H_
#define JZ_SIZE_H_
#include "JZGlobal.h"
class JZDebug;
class JZFRAMEWORK_API JZSize
{
public:
    JZSize();
    JZSize(int w, int h);

    int width() const;
    int height() const;
    void setWidth(int w);
    void setHeight(int h);

    bool isEmpty() const;   // w <= 0 || h <= 0
    bool isValid() const;   // w >= 0 && h >= 0

    // 值相等: 两个 JZSize 比宽高(JS 侧对应 equal(other))
    bool operator==(const JZSize& o) const { return m_w == o.m_w && m_h == o.m_h; }
    bool operator!=(const JZSize& o) const { return !(*this == o); }

private:
    int m_w = 0;
    int m_h = 0;
};
JZDebug operator<<(JZDebug d, const JZSize& rc);

#endif // JZ_SIZE_H_
