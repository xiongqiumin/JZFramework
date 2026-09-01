#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_

#include "JZLayer.h"
#include "JZRect.h"
#include "JZMouseEvent.h"

#include <functional>
#include <string>
#include <vector>

class JZWidget;
class JZGdiBackend;
class JZWindow;

// 显示设备: 纯渲染调度器 (非控件, 不在控件树中)。
// 职责:
//   - render(root, rc): 以 root 为渲染树根遍历子树 (含剔除), 调用各控件
//     paintEvent 收集 JZDrawTask 到 JZLayer;
//   - 交给 JZGdiBackend 回放并提交到屏幕 (渲染后端只负责绘图);
//   - 由 JZWindow::doPaint 调用 (root 传窗口自身), 依赖方向保持 window -> device。
//
// 控件树/绘制任务均为逻辑像素, m_scale (DPI 缩放) 负责逻辑 <-> 物理换算。
class JZDisplayDevice
{
public:
    JZDisplayDevice();
    virtual ~JZDisplayDevice();

    void init(void* hwnd, int width, int height);
    JZLayer* layer();

    // 本轮重绘的脏区 (全局坐标), JZPainter 构造时以此 ∩ 控件 rect 为裁剪
    void setDirtyRect(const JZRect& rc) { m_dirty = rc; }
    const JZRect& dirtyRect() const { return m_dirty; }

    float scale() const;                   // DPI 缩放 (逻辑 -> 物理像素)
    void setScale(float scale);            // DPI 变化 (重建字体)

    // 遍历渲染树 (含剔除) -> 各控件 paintEvent -> backend 渲染并 Present
    void render(JZWidget* root, const JZRect& rc);

private:
    void collectFrontToBack(JZWidget* w, const JZRect& area,
                            std::vector<JZRect>& coverage, std::vector<JZWidget*>& out);

    JZGdiBackend* m_backend = nullptr;
    JZLayer m_layer;
    float m_scale = 1.0f;
    JZRect m_dirty;   // 本轮重绘脏区 (render 时更新)
};

#endif