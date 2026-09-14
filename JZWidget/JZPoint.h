#ifndef JZ_POINT_H_
#define JZ_POINT_H_

class JZPoint
{
public:
    JZPoint();
    JZPoint(int x, int y);

    int x() const;
    int y() const;
    void setX(int x);
    void setY(int y);

    void setPoint(int x, int y);

    bool isNull() const;        // x == 0 && y == 0

    // 值相等: 两个 JZPoint 比坐标(JS 侧对应 equal(other) —— JS/TS 都没有运算符重载, 见 JZFont 的同类写法)
    bool operator==(const JZPoint& o) const { return m_x == o.m_x && m_y == o.m_y; }
    bool operator!=(const JZPoint& o) const { return !(*this == o); }

    JZPoint operator+(const JZPoint& other) const;
    JZPoint operator-(const JZPoint& other) const;
    JZPoint& operator+=(const JZPoint& other);
    JZPoint& operator-=(const JZPoint& other);

private:
    int m_x = 0;
    int m_y = 0;
};

#endif // JZ_POINT_H_
