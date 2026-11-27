#ifndef JZ_D2D_BACKEND_H_
#define JZ_D2D_BACKEND_H_
#include "JZGlobal.h"
#include "JZRenderBackend.h"

#include <memory>

class JZDisplayDevice;
class JZLayer;
class JZImage;

// ============================================================================
// JZD2DBackend: Direct2D 渲染后端 (GPU)
//
// 为什么是 D2D 而不是 OpenGL (完整对比见 .cpp 头部): 本框架的绘制任务是
// "矩形/圆角/线/弧/路径/文本/预乘位图 + 仿射变换 + 轴对齐裁剪", 恰好都是 D2D 的
// 原生图元; OpenGL 则要自己写着色器、三角化、字形图集, 还要与 GDI 的双缓冲/窗口
// 抢所有权。更关键的是像素布局: JZPixmap 已是 **32bppPARGB (BGRA 预乘)**,
// 与 D2D 的 DXGI_FORMAT_B8G8R8A8_UNORM + D2D1_ALPHA_MODE_PREMULTIPLIED 完全一致,
// 贴图上传零转换。
//
// 目标:
//   * target != nullptr (窗口): ID2D1HwndRenderTarget, 硬件加速 (D3D11/DXGI), EndDraw 提交;
//   * target == nullptr (离屏): **不支持**, init 返回 false => JZDisplayDevice 自动回退
//     GDI 后端 (离屏要的是 targetPixels 零拷贝取帧, 那是 CPU 缓冲的形状)。
//
// 与 GDI 后端的语义严格对齐: 同一份 JZLayer 任务列表, 两套后端画出来应当逐像素近似。
//   世界变换 = Scale(dpi) * Translate(ox,oy) * matrix; 几何以控件本地坐标提交;
//   裁剪 = **显示坐标**的轴对齐矩形 (在 identity 变换下 PushAxisAlignedClip);
//   描边内缩半笔宽、圆角半径同步减半笔宽 (Qt/GDI+ 的 drawRect 语义)。
//
// 选择方式: 环境变量 JZ_RENDER_BACKEND=d2d|gdi (默认 gdi, 见 jzCreateRenderBackend)。
// ============================================================================
class JZFRAMEWORK_API JZD2DBackend : public JZRenderBackend
{
public:
    JZD2DBackend();
    ~JZD2DBackend() override;

    bool init(JZDisplayDevice* device, void* target,
              unsigned physicalW, unsigned physicalH) override;
    void destroy() override;

    void render(JZLayer* layer) override;
    void resizeSwapChain(unsigned physicalW, unsigned physicalH) override;
    void rebuildFonts() override;

    // 只有窗口目标: 帧缓冲在 GPU 上, 不提供 CPU 取帧。
    bool snapshot(JZImage& out) override;

    // 进程里 D2D 是否可用 (老系统/被策略禁用时 false, 工厂据此回退 GDI)
    static bool available();

private:
    struct Impl;
    void replay(JZLayer* layer);     // 任务回放 (状态机 + 图元)
    std::unique_ptr<Impl> m_d;
};

#endif // !JZ_D2D_BACKEND_H_
