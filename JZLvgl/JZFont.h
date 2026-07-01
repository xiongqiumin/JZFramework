#ifndef JZ_FONT_H_
#define JZ_FONT_H_

class JZFont
{
public:
    JZFont() = default;
    explicit JZFont(int size) : m_size(size) {}

    void setSize(int size)      { m_size = size; }
    int size() const            { return m_size; }

private:
    int m_size = 14;
};

#endif // JZ_FONT_H_
