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

    // 裁剪属于绘制流状态, 变更时发 Push。
    // setClip(rc): rc 为**控件本地坐标**, 与 drawXxx/fillRect 等各接口一致,
    // paintEvent 内部一律用这个; 内部自动平移到显示(窗口)坐标再写入绘制流。
    // 初始裁剪 (脏区 ∩ 控件全局 rect) 在构造时已设定, 一般无需再调。
    void setClip(const JZRect& clip);

    void drawLine(int x1, int y1, int x2, int y2);
    // 描边矩形/圆角矩形 (用当前 pen), 语义对齐 Qt QPainter::drawRect:
    // 边框恰好包住与 fillRect(x,y,w,h) 相同的像素矩形外圈 [x..x+w-1]×[y..y+h-1],
    // 线条整体落在矩形内 (几何按半笔宽内缩), 因此不会越过控件 clip 造成右/下缺边。
    void drawRect(int x, int y, int w, int h);
    void drawRoundRect(int x, int y, int w, int h, int radius);

    // 填充矩形 (对齐 Qt QPainter::fillRect): 填充像素 [x..x+w-1]×[y..y+h-1],
    // 与 JZRect 闭区间语义 (right=x+w-1) 一致。同参数下 drawRect 描其外圈边框。
    void fillRect(const JZRect &rc, const JZColor& color);
    void fillRect(int x, int y, int w, int h, const JZColor& color);

    // 填充圆角矩形 (只填充不描边, 同参数下 drawRoundRect 还会按当前 brush 填内部)。
    // 真圆 = 正方形且 radius == 边长/2 (如单选钮内点)。
    void fillRoundRect(int x, int y, int w, int h, int radius, const JZColor& color);

    // 角度单位: 1/10 度 (与 LVGL 一致: 0=3点方向, 90=6点方向, 顺时针递增)
    void drawArc(int x, int y, int radius, int startAngle, int endAngle);
    void drawText(int x, int y, const char* text);

    void drawImage(const JZImage& img, int x, int y, int w, int h);
    // 贴显示用位图 (JZPixmap): 同一张图反复绘制时用它 —— 转换已在 JZPixmap::fromImage
    // 时做完, 每次绘制不再有任何逐像素开销 (drawImage 路径会做同样的转换并缓存)。
    void drawPixmap(const JZPixmap& pixmap, int x, int y, int w, int h);

private:
    // 粗略裁剪: 图元显示坐标包围盒与裁剪区(优先用 m_clip, 否则用屏幕)不相交则丢弃。
    bool isCulled(const JZRect& displayBounds) const;

    // 把当前完整状态 (pen/brush/font/clip) 以 Push 任务写入绘制流。
    void pushState();

    // 祖先裁剪: 向上找到开启 setClipChildren() 的祖先控件 (如滚动区 viewport),
    // 把显示坐标裁剪区收缩到其显示范围内; widget 为该 painter 所属控件。
    JZRect applyAncestorClips(JZWidget* widget, const JZRect& globalClip) const;

    struct State {
        JZPen pen;
        JZBrush brush;
        JZFont font;
    };

    JZLayer* m_layer = nullptr;
    JZWidget* m_widget = nullptr;

    // 绘制原点: 构造时按 widget 全局坐标自动设置, 内部用于把本地坐标平移到
    // 显示坐标 (drawXxx/setClip 通用)。不对外开放, 外部不可调整。
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
