#ifndef JZ_RENDER_BACKEND_H_
#define JZ_RENDER_BACKEND_H_
#include "JZGlobal.h"

class JZDisplayDevice;
class JZLayer;
class JZImage;

// ==================== 渲染后端接口 ====================
// 只做一件事: 把 JZLayer 里的绘制任务回放到目标并提交。
//   - target 非空: 平台窗口 (Win32 即 HWND), 回放后提交上屏;
//   - target 为空: **离屏** —— 只维护自己的帧缓冲, 不提交 (取帧走 snapshot)。
//
// 现有实现: JZGdiBackend (GDI+, 复用同一份任务回放逻辑)。
// 后续 GPU / 纯软光栅 / 虚拟设备后端按同一接口接入 JZDisplayDevice 即可,
// 控件树、JZPainter、JZLayer 均不需改动 (绘制调用只产生任务, 不直接写屏)。
class JZFRAMEWORK_API JZRenderBackend
{
public:
    virtual ~JZRenderBackend() {}

    // 绑定目标 + 建帧缓冲 (尺寸为物理像素)。
    // target == nullptr 表示离屏: 实现需自建与窗口无关的兼容 DC (如 CreateCompatibleDC(nullptr))。
    virtual bool init(JZDisplayDevice* device, void* target,
                      unsigned physicalW, unsigned physicalH) = 0;
    virtual void destroy() = 0;

    // 回放本帧绘制任务并提交; 结束后清空任务列表。
    virtual void render(JZLayer* layer) = 0;

    // 目标尺寸变化 (重建帧缓冲)。
    virtual void resizeSwapChain(unsigned physicalW, unsigned physicalH) = 0;

    // DPI/字号变化钩子 (按任务即时创建字体的后端可为空实现)。
    virtual void rebuildFonts() = 0;

    // 把当前帧缓冲拷成 JZImage (RGBA8888)。后端不支持时返回 false。
    virtual bool snapshot(JZImage& out) { (void)out; return false; }
};

#endif // JZ_RENDER_BACKEND_H_
