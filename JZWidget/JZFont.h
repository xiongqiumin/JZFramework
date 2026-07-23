#ifndef JZ_FONT_H_
#define JZ_FONT_H_

class JZFont
{
public:
    JZFont() = default;
    explicit JZFont(int size);

    void setSize(int size);
    int size() const;

private:
    int m_size = 14;
};

#endif // JZ_FONT_H_
