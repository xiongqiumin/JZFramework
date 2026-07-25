#ifndef JZ_DRAW_TASK_H_
#define JZ_DRAW_TASK_H_

#include "JZRect.h"
#include "JZColor.h"
#include "JZImage.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// 绘制指令基类。由 JZPainter 在 paintEvent 中产生 (派生类型), 经 JZLayer::redraw()
// 翻译成对应的 LVGL v9 draw 调用 (lv_draw_xxx), 再交给 draw unit 渲染。
// 用继承而非"大 struct + type 字段", 让每种指令只携带自己需要的字段;
// 提交统一用 std::shared_ptr<JZDrawTask> (见 JZLayer::addTask)。
struct JZDrawTask
{
    enum Type {
        None,
        Line,    // 线段 (p1 -> p2), 支持任意角度
        Rect,    // 填充矩形
        Border,  // 描边矩形 (用 pen 宽度画四条边)
        Arc,     // 圆弧 (center + radius + start/end angle)
        Text,    // 文本 (需要字体引擎)
        Image,   // 图像混合
        Changed  // pen/brush/font 状态变更标记 (save/restore 均产生一次状态快照)
    };

    explicit JZDrawTask(Type t = None) : type(t) {}
    virtual ~JZDrawTask() = default;

    Type type;
    JZRect clip;           // 脏区裁剪 (刷新时按当前脏区设置)
    uint8_t opacity = 255; // 不透明度 0..255 (255 不透明)
};

// ---- Line ----
struct JZLineTask : JZDrawTask {
    JZLineTask() : JZDrawTask(Line) {}
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    JZPen pen;            // 线宽/颜色统一从 pen 取
};

// ---- Rect (填充) ----
struct JZFillRectTask : JZDrawTask {
    JZFillRectTask() : JZDrawTask(Rect) {}
    JZRect area;
    JZColor color;
    int radius = 0;
};

// ---- Border (描边) ----
struct JZBorderTask : JZDrawTask {
    JZBorderTask() : JZDrawTask(Border) {}
    JZRect area;
    JZPen pen;            // 边宽/颜色统一从 pen 取
    int radius = 0;
};

// ---- Arc ----
struct JZArcTask : JZDrawTask {
    JZArcTask() : JZDrawTask(Arc) {}
    int arcCx = 0, arcCy = 0;
    int arcRadius = 0;
    int arcStartAngle = 0;
    int arcEndAngle = 3600; // 默认整圆 (360 度, 单位 1/10 度)
    bool arcRounded = false;
    JZPen pen;            // 弧线宽/颜色统一从 pen 取
};

// ---- Text ----
struct JZTextTask : JZDrawTask {
    JZTextTask() : JZDrawTask(Text) {}
    JZRect area;
    JZColor color;
    std::string text;
    JZFont font;          // 字号/字体从 JZFont 取
};

// ---- Image ----
struct JZImageTask : JZDrawTask {
    JZImageTask() : JZDrawTask(Image) {}
    JZRect area;
    std::shared_ptr<JZImageData> image;
};

// ---- Changed 状态快照 ----
// JZPainter::save() / restore() 都把当前 pen/brush/font 拷进一个 Changed 任务。
// JZLayer::redraw() 对其做无操作 (状态由 JZPainter 自身栈维护), 这里保留快照仅用于
// 绘制流的序列化/回放。
struct JZChangedTask : JZDrawTask {
    JZChangedTask() : JZDrawTask(Changed) {}
    JZPen savedPen;
    JZBrush savedBrush;
    JZFont savedFont;
};

#endif // JZ_DRAW_TASK_H_
