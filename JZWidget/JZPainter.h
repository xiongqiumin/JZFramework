#ifndef JZ_PAINTER_H_
#define JZ_PAINTER_H_

#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZColor.h"
#include "JZImage.h"
#include "JZLayer.h"
#include "JZRect.h"

class JZWidget;

// 绘制入口: 在 paintEvent 中由 JZDisplayDevice 创建并下发。
// 所有绘制调用不直接写屏, 而是向当前图层追加 JZDrawTask (等价 LVGL 在 DRAW 事件回调里 lv_draw_xxx)。
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

    // 绘制原点 (由刷新遍历按 widget 绝对坐标设置)
    void setOrigin(int ox, int oy) { m_ox = ox; m_oy = oy; }
    void setClip(const JZRect& clip) { m_clip = clip; }

    void drawLine(int x1, int y1, int x2, int y2);
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);
    void fillRect(int x, int y, int w, int h, const JZColor& color);

    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    void drawImage(const JZImage& img, int x, int y, int w, int h);

private:
    JZLayer* m_layer = nullptr;
    
    int m_ox = 0;
    int m_oy = 0;
    JZRect m_clip;
    JZPen m_pen;
    JZBrush m_brush;
    JZFont m_font;
};

#endif
