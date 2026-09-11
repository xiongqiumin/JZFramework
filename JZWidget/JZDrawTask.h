#ifndef JZ_DRAW_TASK_H_
#define JZ_DRAW_TASK_H_

#include "JZRect.h"
#include "JZColor.h"
#include "JZImage.h"
#include "JZPixmap.h"
#include "JZPen.h"
#include "JZBrush.h"
#include "JZFont.h"
#include "JZOptional.h"
#include "JZPainterPath.h"
#include "JZMatrix2D.h"

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
        Image,   // JZImage 贴图 (像素布局由后端自行转换/缓存)
        Pixmap,  // JZPixmap 贴图 (已是后端原生布局, 直接贴)
        Path,    // 矢量路径 (JZPainterPath), 可描边/填充
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

// ---- Pixmap (已转成后端原生布局的显示用位图) ----
struct JZPixmapTask : JZDrawTask {
    JZPixmapTask() : JZDrawTask(Pixmap) {}
    JZRect area;
    std::shared_ptr<JZPixmapData> pixmap;
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
    // 控件本地变换: 以控件局部坐标系为基准 (原点=控件左上角, 每个控件绘制前为单位矩阵),
    // 由控件作者经 setTransform/translate/rotate/scale 设置, 不含框架的放置偏移。
    JZMatrix2D matrix;
    // 框架放置偏移 (控件在图层/窗口坐标中的位置, 由 JZPainter 构造时设定, 控件作者无感)。
    // 后端世界变换 = Scale(s) * Translate(ox,oy) * matrix。
    int ox = 0;
    int oy = 0;
};

struct JZDrawPopTask : JZDrawTask {
    JZDrawPopTask() : JZDrawTask(Pop) {}
};

// ---- Path (矢量路径: 描边 +/或 填充) ----
struct JZPathTask : JZDrawTask {
    JZPathTask() : JZDrawTask(Path) {}
    JZPainterPath path;
    bool stroke = false;   // 用当前 pen 描边
    bool fill = false;     // 填充 (fillColor 有值取其色, 否则取当前 brush 色)
    JZOptional<JZColor> fillColor;  // 显式填充色 (如 fillRect 带色重载); 空=用 brush
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
    JZOptional<JZMatrix2D> matrix;   // 非栈式修改当前本地变换 (setTransform/translate/rotate/scale)
};

#endif // JZ_DRAW_TASK_H_
