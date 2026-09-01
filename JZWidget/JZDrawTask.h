#ifndef JZ_DRAW_TASK_H_
#define JZ_DRAW_TASK_H_

#include "JZRect.h"
#include "JZColor.h"
#include "JZImage.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZOptional.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

// 绘制指令基类。由 JZPainter 在 paintEvent 中产生 (派生类型), 交给渲染后端回放。
// 任务只携带几何与调用参数 (如 fillRect 的 color); 样式 (pen/brush/font) 与裁剪
// (clip) 属于绘制流状态, 由三类任务维护:
//   Push:   保存绘制流当前状态, 并应用本任务携带的新状态 (栈语义, 与 Pop 配对);
//   Change: 非栈语义, 直接修改当前状态 (仅 Some 的字段生效, 不影响 Push/Pop 配对);
//   Pop:    无参数, 回退到匹配 Push 之前的状态。
// 提交统一用 std::shared_ptr<JZDrawTask> (见 JZLayer::addTask)。
struct JZDrawTask
{
    enum Type {
        None,
        Line,    // 线段 (p1 -> p2), 支持任意角度, 用当前 pen
        Rect,    // 填充矩形, color 为调用参数
        Border,  // 描边矩形 (用当前 pen 画四条边)
        Arc,     // 圆弧 (center + radius + start/end angle), 用当前 pen
        Text,    // 文本, 用当前 font + pen 颜色
        Image,   // 图像混合
        Push,    // 状态压栈: 保存当前状态并应用携带的新状态 (pen/brush/font/clip)
        Change,  // 状态修改 (非栈): 设置当前状态中 Some 的字段
        Pop      // 状态出栈: 回退到匹配 Push 之前的状态
    };

    explicit JZDrawTask(Type t = None) : type(t) {}
    virtual ~JZDrawTask() = default;

    Type type;
    uint8_t opacity = 255; // 不透明度 0..255 (255 不透明)
};

// ---- Line ----
struct JZLineTask : JZDrawTask {
    JZLineTask() : JZDrawTask(Line) {}
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

// ---- Rect (填充) ----
struct JZFillRectTask : JZDrawTask {
    JZFillRectTask() : JZDrawTask(Rect) {}
    JZRect area;
    JZColor color;        // 调用参数 (fillRect 显式传入, 不走状态机)
    int radius = 0;
};

// ---- Border (描边) ----
struct JZBorderTask : JZDrawTask {
    JZBorderTask() : JZDrawTask(Border) {}
    JZRect area;
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
};

// ---- Text ----
struct JZTextTask : JZDrawTask {
    JZTextTask() : JZDrawTask(Text) {}
    JZRect area;
    std::string text;
};

// ---- Image ----
struct JZImageTask : JZDrawTask {
    JZImageTask() : JZDrawTask(Image) {}
    JZRect area;
    std::shared_ptr<JZImageData> image;
};

// ---- Push/Pop 状态机 ----
// Push: 保存绘制流当前状态, 并应用本任务携带的新状态 (pen/brush/font/clip)。
// Pop:  无参数, 回退到匹配 Push 之前的状态。
// JZPainter 构造/save() 发 Push; restore()/析构发 Pop (严格配对)。
struct JZDrawPushTask : JZDrawTask {
    JZDrawPushTask() : JZDrawTask(Push) {}
    JZPen pen;
    JZBrush brush;
    JZFont font;
    JZRect clip;
};

struct JZDrawPopTask : JZDrawTask {
    JZDrawPopTask() : JZDrawTask(Pop) {}
};

// ---- Change 状态修改 (非栈) ----
// 直接设置绘制流当前状态: 仅 Some 的字段生效, None 的字段保持不变。
// 不影响 Push/Pop 配对 (setPen/setBrush/setFont/setClip 等 setter 使用)。
struct JZDrawChangeTask : JZDrawTask {
    JZDrawChangeTask() : JZDrawTask(Change) {}
    JZOptional<JZPen> pen;
    JZOptional<JZBrush> brush;
    JZOptional<JZFont> font;
    JZOptional<JZRect> clip;
};

#endif // JZ_DRAW_TASK_H_
