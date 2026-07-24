#ifndef JZ_LAYER_H_
#define JZ_LAYER_H_

#include "JZDrawTask.h"
#include <vector>

// 图层: 持有一块像素缓冲 + 本帧待执行的 draw_task 列表。
// 等价于 LVGL 的 lv_layer_t, 但 task 列表由 JZWidget 遍历产生(见 JZDisplayDevice::refresh),
// 不再由 LVGL 的 obj 树遍历产生。
class JZLayer
{
public:
    explicit JZLayer();

    void addTask(const JZDrawTask& t) { m_tasks.push_back(t); }
    void clearTasks() { m_tasks.clear(); }
    const std::vector<JZDrawTask>& tasks() const { return m_tasks; }

    // 执行本帧所有 task, 写进 buffer (每个 task 按 area∩clip∩buffer 裁剪)
    void dispatch();

private:    
    std::vector<JZDrawTask> m_tasks;    
};

#endif // JZ_LAYER_H_
