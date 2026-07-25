#ifndef JZ_LAYER_H_
#define JZ_LAYER_H_

#include "JZDrawTask.h"
#include <vector>

// 图层: 持有一块像素缓冲 (LVGL 的 lv_layer_t->draw_buf) + 本帧待执行的 draw_task 列表。
// 等价于 LVGL 的 lv_layer_t, 但 task 列表由 JZWidget 遍历产生(见 JZDisplayDevice::refresh),
// 不再由 LVGL 的 obj 树遍历产生。
typedef struct _lv_layer_t lv_layer_t;
typedef struct _lv_display_t lv_display_t;

class JZLayer
{
public:
    explicit JZLayer();

    void addTask(std::shared_ptr<JZDrawTask> t) { m_tasks.push_back(std::move(t)); }
    void clearTasks() { m_tasks.clear(); }
    const std::vector<std::shared_ptr<JZDrawTask>>& tasks() const { return m_tasks; }
    
    void redraw(lv_layer_t *layer);

private:
    void applyClip(lv_layer_t *layer, const JZRect &clip);

    std::vector<std::shared_ptr<JZDrawTask>> m_tasks;
};

#endif // JZ_LAYER_H_
