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
    ~JZPainter();   // 发 Pop 平衡构造时的 Push (未配对的 save 一并自动闭合)

    void setPen(const JZPen& pen);
    JZPen pen() const;

    void setBrush(const JZBrush& brush);
    JZBrush brush() const;

    void setFont(const JZFont& font);
    JZFont font() const;

    // 保存/恢复绘制状态 (pen/brush/font)。save 向绘制流追加 Push, restore 追加 Pop,
    // 与构造/析构的 Push/Pop 共同保证绘制流状态机严格配对。
    void save();
    void restore();

    // 绘制原点: 构造时按 widget 全局坐标自动设置, 提供 setOrigin 供手动偏移。
    void setOrigin(int ox, int oy) { m_ox = ox; m_oy = oy; }
    void setClip(const JZRect& clip);   // 裁剪属于绘制流状态, 变更时发 Push

    void drawLine(int x1, int y1, int x2, int y2);
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);

    void fillRect(const JZRect &rc, const JZColor& color);
    void fillRect(int x, int y, int w, int h, const JZColor& color);

    // 角度单位: 1/10 度 (与 LVGL 一致: 0=3点方向, 90=6点方向, 顺时针递增)
    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    void drawImage(const JZImage& img, int x, int y, int w, int h);

private:
    // 粗略裁剪: 图元显示坐标包围盒与裁剪区(优先用 m_clip, 否则用屏幕)不相交则丢弃。
    bool isCulled(const JZRect& displayBounds) const;

    // 把当前完整状态 (pen/brush/font/clip) 以 Push 任务写入绘制流。
    void pushState();

    struct State {
        JZPen pen;
        JZBrush brush;
        JZFont font;
    };

    JZLayer* m_layer = nullptr;

    int m_ox = 0;
    int m_oy = 0;
    JZRect m_clip;
    JZRect m_screen;            // 屏幕可见区域, 用于完全出屏的粗略剔除
    std::vector<State> m_stack; // save/restore 状态栈

    int m_streamDepth = 0;      // 已写入绘制流、待析构 Pop 平衡的 Push 数

    JZPen m_pen;
    JZBrush m_brush;
    JZFont m_font;
};

#endif
