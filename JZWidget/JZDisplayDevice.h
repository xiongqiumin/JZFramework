#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZWidget.h"
#include "JZLayer.h"
#include "JZRect.h"
#include "JZColor.h"

#include <vector>

// 显示设备: 拥有屏缓冲(=layer 的 buffer) + 脏区域集合。
// 等价于 LVGL 的 lv_display_t, 但:
//   - 不创建 screen lv_obj;
//   - 不持有 obj 树刷新遍历, 改为在 refresh() 里遍历 JZWidget 树(含剔除);
//   - 脏区域合并沿用单个 JZRect 的 united()(等价 disp->inv_areas[] + lv_refr_join_area)。
class JZDisplayDevice : public JZWidget
{
public:
    JZDisplayDevice(void* display);
    JZDisplayDevice(int w, int h);
    ~JZDisplayDevice();

    // 登记脏矩形 (等价 lv_inv_area)。只追加, 不触发绘制。
    void invalidate(const JZRect& rect);

    // 一次性刷新: 合并脏区 -> 逐区剔除遍历发 paintEvent -> 生成 draw_task -> dispatch 混合 -> flush
    void refresh();

    // 递交到真实硬件 (SDL/FrameBuffer/...)。默认空实现, 由平台子类重写。
    virtual void flush();

    void* frameBuffer();
    int width() const;
    int height() const;

    void setBackgroundColor(const JZColor& c);
    JZColor backgroundColor() const;

private:
    static void collectFrontToBack(JZWidget* w, const JZRect& area, std::vector<JZRect>& coverage,
                                   std::vector<JZWidget*>& out);
    static std::vector<JZWidget*> cullList(JZWidget* root, const JZRect& area);

    JZLayer m_layer;
    JZRect m_dirtyRect;            // 累计脏区域 (united 合并, 等价 disp->inv_areas[])
    JZPixel m_bg = {255, 0, 0, 0}; // 黑底
};

#endif
