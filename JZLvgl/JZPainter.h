#ifndef JZ_PAINTER_H_
#define JZ_PAINTER_H_

#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZColor.h"
#include "JZImage.h"

class JZWidget;

// Forward-declare LVGL types
typedef struct _lv_layer_t lv_layer_t;
typedef struct _lv_font_t lv_font_t;

class JZPainter
{
public:
    explicit JZPainter(JZWidget* widget);

    void setPen(const JZPen& pen);
    JZPen pen() const;
    void setBrush(const JZBrush& brush);
    JZBrush brush() const;
    void setFont(const JZFont& font);
    JZFont font() const;

    void drawLine(int x1, int y1, int x2, int y2);
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);
    void fillRect(int x, int y, int w, int h, const JZColor& color);
    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    // 绘制图像(仅支持已解码的 dsc 图像; 文件路径图像会被忽略)
    void drawImage(const JZImage& img, int x, int y, int w, int h);

    static const lv_font_t* resolveFont(const JZFont& font);

private:
    lv_layer_t* m_layer;
    int m_ox = 0;   // 绘制原点 x(绝对坐标, 由 widget 提供)
    int m_oy = 0;   // 绘制原点 y
    JZPen m_pen;
    JZBrush m_brush;
    JZFont m_font;
};

#endif
