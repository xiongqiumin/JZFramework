#ifndef JZ_DRAW_TASK_H_
#define JZ_DRAW_TASK_H_

#include "JZRect.h"
#include "JZDrawBuffer.h"

// 一次绘制指令 (等价 LVGL 的 lv_draw_task_t)。
// 由 JZWidget::paintEvent 经 JZPainter 生成, 由 JZLayer::dispatch 在刷新时执行(写像素)。
enum class JZDrawTaskType {
    Rect,    // 填充矩形
    Border   // 描边矩形 (用 borderWidth 画四条边)
};

struct JZDrawTask {
    JZDrawTaskType type = JZDrawTaskType::Rect;
    JZRect area;      // 显示坐标下的矩形
    JZRect clip;      // 脏区裁剪 (刷新时按当前脏区设置)
    JZPixel color;    // 填充/描边颜色 (a=255 表示不透明)
    int radius = 0;        // 圆角 (TODO: JZLayer 暂按直角处理)
    int borderWidth = 1;   // Border 类型边宽
};

#endif // JZ_DRAW_TASK_H_
