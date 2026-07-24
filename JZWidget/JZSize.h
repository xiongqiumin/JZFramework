#ifndef JZ_SIZE_H_
#define JZ_SIZE_H_

class JZSize
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

private:
    int m_w = 0;
    int m_h = 0;
};

#endif // JZ_SIZE_H_
