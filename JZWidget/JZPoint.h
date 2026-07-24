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

    JZPoint operator+(const JZPoint& other) const;
    JZPoint operator-(const JZPoint& other) const;
    JZPoint& operator+=(const JZPoint& other);
    JZPoint& operator-=(const JZPoint& other);

private:
    int m_x = 0;
    int m_y = 0;
};

#endif // JZ_POINT_H_
