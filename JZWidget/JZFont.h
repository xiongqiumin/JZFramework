#ifndef JZ_FONT_H_
#define JZ_FONT_H_

#include <string>

// 字体描述 (仿 Qt QFont): 字体族 + 像素字号 + 风格 (粗体/斜体/下划线/删除线)。
// 仅携带描述, 真正的 GDI+ 字体对象由 JZGdiBackend 按此描述惰性创建。
class JZFont
{
public:
    enum Style {
        Normal     = 0,
        Bold       = 1 << 0,
        Italic     = 1 << 1,
        Underline  = 1 << 2,
        StrikeOut  = 1 << 3,
    };

    JZFont() = default;
    explicit JZFont(int size);
    JZFont(const std::string& family, int size, int style = Normal);

    void setFamily(const std::string& family);
    const std::string& family() const;

    void setSize(int size);
    int size() const;

    void setStyle(int style);
    int style() const;

    void setBold(bool on);
    bool bold() const;
    void setItalic(bool on);
    bool italic() const;
    void setUnderline(bool on);
    bool underline() const;
    void setStrikeOut(bool on);
    bool strikeOut() const;

    bool operator==(const JZFont& o) const;
    bool operator!=(const JZFont& o) const;

private:
    std::string m_family;   // 空 = 默认字体 (由渲染后端决定, 当前为微软雅黑)
    int m_size = 14;        // 像素字号
    int m_style = Normal;   // Style 位或
};

#endif // JZ_FONT_H_
