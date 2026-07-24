#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"
#include "JZLayer.h"
#include "JZRect.h"
#include "JZColor.h"

#include <vector>

// 注意: lv_display_t / lv_event_t 等类型由 JZWidget.h 间接包含的 lvgl.h 提供,
// 不要再前向声明, 否则会与 LVGL 的 typedef 冲突 (C2371 重定义)。

// 显示设备: 拥有屏缓冲(=layer 的 buffer) + 脏区域集合。
// 等价于 LVGL 的 lv_display_t, 但:
//   - 不创建 screen lv_obj;
//   - 不持有 obj 树刷新遍历, 改为在 refresh() 里遍历 JZWidget 树(含剔除);
//   - 脏区域合并沿用单个 JZRect 的 united()(等价 disp->inv_areas[] + lv_refr_join_area)。

typedef struct _lv_display_t lv_display_t;
class JZDisplayDevice : public JZWidget
{
public:
    JZDisplayDevice(lv_display_t* display);
    JZDisplayDevice(int w, int h);
    ~JZDisplayDevice();
    
    JZLayer *layer();
    int width() const;
    int height() const;

private:
    static void collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage,
                                   std::vector<JZWidget*>& out);
    static std::vector<JZWidget*> cullList(JZWidget* root, const JZRect& area);

    lv_display_t* m_disp = nullptr; // 外部传入的 LVGL display; 为空则走软件渲染路径
    JZLayer m_layer;
};

#endif
