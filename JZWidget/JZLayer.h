#ifndef JZ_LAYER_H_
#define JZ_LAYER_H_
#include "JZDrawTask.h"
#include <vector>

// 图层: 本帧待执行的 draw_task 列表 (渲染后端无关)。
// 任务由 JZWidget 树遍历产生 (见 JZDisplayDevice::redraw), 渲染后端
// (JZGdiBackend) 负责把任务列表翻译为具体绘制调用后清空。
class JZFRAMEWORK_API JZLayer
{
public:
    JZLayer();

    void addTask(std::shared_ptr<JZDrawTask> t) { m_tasks.push_back(std::move(t)); }
    void clearTasks() { m_tasks.clear(); }
    const std::vector<std::shared_ptr<JZDrawTask>>& tasks() const { return m_tasks; }

private:
    std::vector<std::shared_ptr<JZDrawTask>> m_tasks;
};

#endif // JZ_LAYER_H_
