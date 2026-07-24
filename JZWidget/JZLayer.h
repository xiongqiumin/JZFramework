#ifndef JZ_LAYER_H_
#define JZ_LAYER_H_

#include "JZDrawBuffer.h"
#include "JZDrawTask.h"
#include <vector>

#include "lvgl.h"   // lv_layer_t 等 LVGL 类型 (JZLayer 直接用 LVGL 绘制)

// 图层: 持有一块像素缓冲 + 本帧待执行的 draw_task 列表。
// 等价于 LVGL 的 lv_layer_t, 但 task 列表由 JZWidget 遍历产生(见 JZDisplayDevice::refresh),
// 不再由 LVGL 的 obj 树遍历产生。
class JZLayer
{
public:
    explicit JZLayer(int w = 0, int h = 0);

    JZDrawBuffer* buffer() { return &m_buf; }
    const JZDrawBuffer* buffer() const { return &m_buf; }

    void resize(int w, int h);

    void addTask(const JZDrawTask& t) { m_tasks.push_back(t); }
    void clearTasks() { m_tasks.clear(); }
    const std::vector<JZDrawTask>& tasks() const { return m_tasks; }

    // 执行本帧所有 task, 写进 buffer (每个 task 按 area∩clip∩buffer 裁剪)
    void dispatch();

    // LVGL 后端: 在屏幕刷新回调里直接按 LVGL 的格式绘制到给定 layer。
    // 等价于参考 demo 的 draw_event_cb(), 用 m_frame 当动画帧计数。
    void drawTask(lv_layer_t* layer);

    // 推进一帧动画计数 (refresh 时调用)
    void nextFrame() { ++m_frame; }
    int64_t frame() const { return m_frame; }

private:
    JZDrawBuffer m_buf;
    std::vector<JZDrawTask> m_tasks;
    int64_t m_frame = 0;   // 动画帧计数, 等价参考 demo 的 g_frame
};

#endif // JZ_LAYER_H_
