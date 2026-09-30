#ifndef JZ_DISPLAY_DEVICE_H_
#define JZ_DISPLAY_DEVICE_H_
#include "JZLayer.h"
#include "JZRect.h"
#include "JZMouseEvent.h"

#include <functional>
#include <string>
#include <vector>

class JZWidget;
class JZRenderBackend;
class JZImage;
class JZWindow;

// 显示设备: 纯渲染调度器 (非控件, 不在控件树中)。
// 职责:
//   - render(root, rc): 以 root 为渲染树根遍历子树 (含剔除), 调用各控件
//     paintEvent 收集 JZDrawTask 到 JZLayer;
//   - 交给渲染后端 (JZRenderBackend; 默认 JZGdiBackend) 回放并提交 (渲染后端只负责绘图);
//   - 由 JZWindow::doPaint 调用 (root 传窗口自身), 依赖方向保持 window -> device。
//
// 多后端: 后端可经 setRenderBackend 替换 (GDI+ / GPU / 离屏虚拟后端同一接口);
// 后端 target 允许为空 => 离屏渲染 (无窗口), 见 initOffscreen。
//
// 控件树/绘制任务均为逻辑像素, m_scale (DPI 缩放) 负责逻辑 <-> 物理换算。
class JZFRAMEWORK_API JZDisplayDevice
{
public:
    JZDisplayDevice();
    virtual ~JZDisplayDevice();

    // 绑定平台窗口 (Win32: hwnd 即 HWND) + 建帧缓冲; 默认后端。
    void init(void* hwnd, int width, int height);
    // 离屏初始化: 不绑定任何窗口, 只建自己的帧缓冲 (取帧见 snapshot)。
    void initOffscreen(int width, int height);

    // 替换渲染后端 (接管所有权; 传 nullptr = 恢复默认后端)。须在 init 之前调用。
    void setRenderBackend(JZRenderBackend* backend);
    JZRenderBackend* renderBackend() const { return m_backend; }

    // 把当前帧缓冲拷成 JZImage (RGBA8888); 后端不支持时返回 false。
    bool snapshot(JZImage& out);

    JZLayer* layer();

    // 本轮重绘的脏区 (全局坐标), JZPainter 构造时以此 ∩ 控件 rect 为裁剪
    void setDirtyRect(const JZRect& rc) { m_dirty = rc; }
    const JZRect& dirtyRect() const { return m_dirty; }

    float scale() const;                   // DPI 缩放 (逻辑 -> 物理像素)
    void setScale(float scale);            // DPI 变化 (重建字体)

    // 遍历渲染树 (含剔除) -> 各控件 paintEvent -> backend 渲染并 Present
    void render(JZWidget* root, const JZRect& rc);

    // 原生窗口尺寸变化 (WM_SIZE): 同步后端交换链尺寸 (物理像素)
    void resize(int width, int height);

private:
    // ancClip: 祖先 setClipChildren 控件收窄后的可绘制区 (窗口坐标), 参与剔除判定
    // (否则视口内被剪裁的超大子控件会错误覆盖掉它身后的兄弟控件, 如滚动条)
    void collectFrontToBack(JZWidget* w, const JZRect& area,
                            std::vector<JZRect>& coverage, std::vector<JZWidget*>& out,
                            const JZRect& ancClip);

    JZRenderBackend* m_backend = nullptr;
    JZLayer m_layer;
    float m_scale = 1.0f;
    JZRect m_dirty;   // 本轮重绘脏区 (render 时更新)
};

#endif
