#ifndef JZ_GDI_BACKEND_H_
#define JZ_GDI_BACKEND_H_
#include "JZGlobal.h"
#include "JZRenderBackend.h"
#include <memory>
#include <vector>

class JZDisplayDevice;
class JZLayer;
class JZImageData;
class JZPixmapData;
class JZImage;

// ==================== GDI+ 渲染后端 (纯绘图, 临时方案) ====================
// 只负责一件事: 把 JZLayer 的 JZDrawTask 列表画出来并提交到屏幕。
//   - init(): 绑定 hwnd (由 JZWindow 创建并所有), 创建同尺寸内存位图做双缓冲;
//   - render(): 任务回放到内存位图 -> BitBlt 提交到窗口;
//   - 不创建窗口、不处理消息、不消费任何输入 (窗口与消息在 JZWindow)。
//
// (原 ImGui/DX11 后端 JZImGuiBackend 已移除; 后续如需 GPU 渲染,
//  按同样的接口实现一个新后端接入 JZDisplayDevice 即可。)
//
// 坐标系: 绘制任务为逻辑像素, 回放时 * device->scale() -> 物理像素。
class JZFRAMEWORK_API JZGdiBackend : public JZRenderBackend
{
public:
    JZGdiBackend();
    ~JZGdiBackend() override;

    // 绑定目标 + 建双缓冲位图 (hwnd / 尺寸由 JZDisplayDevice 提供)。
    // hwnd == nullptr = 离屏: 用屏幕兼容 DC 建 DIB, render 不提交上屏 (取帧走 snapshot)。
    bool init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH) override;
    void destroy() override;

    // 字体度量引擎 (GDI+) 的进程级申请/释放, 与窗口无关、可独立调用:
    //   - 引用计数, 与 GDI+ 生命周期严格对称 (init()/destroy() 内部也走这两个函数);
    //   - 供 JZDriver 在**任何 UI 构造之前**预先注册到 JZFontMetrics。
    //     否则首次布局只能走 JZFontMetrics::estimateWidth 估算 (ASCII 仅 0.55em),
    //     控件会系统性偏窄、文字被裁; 而引擎注册本身不触发重排, 事后无法自动纠正。
    static bool acquireFontEngine();
    static void releaseFontEngine();

    // 渲染一帧: 回放 layer 中的绘制任务; 有窗口则 BitBlt 提交上屏, 离屏则只留在帧缓冲。
    // 结束后清空任务列表。
    void render(JZLayer* layer) override;

    // 目标尺寸变化 (重建内存位图)。
    void resizeSwapChain(unsigned physicalW, unsigned physicalH) override;

    // 接口占位: GDI+ 字体按任务即时创建, DPI 变化无需重建。
    void rebuildFonts() override;

    // 把当前帧缓冲 (DIB) 拷成 JZImage (BGRA -> RGBA8888)。离屏/取帧用。
    bool snapshot(JZImage& out) override;

    // 帧缓冲像素的零拷贝视图 (DIB 是 top-down 32bpp, stride = w*4, 布局即 PARGB)。
    // 显示侧据此直接贴这块缓冲, 不做 RGBA 转换 —— 见 JZDisplayDevice::targetPixmap。
    const uint8_t* targetPixels(int& w, int& h, int& stride) const override;

    // GDI+ 进程级生命周期 (与窗口无关, 仅管理 GDI+ 不碰字体度量引擎):
    // 供离屏绘制 (JZPainter 作用于 JZImage) 在没有任何窗口存活时也能起 GDI+。
    // 与窗口侧的 acquireFontEngine/releaseFontEngine 引用计数相互独立、互不干扰。
    static bool ensureGdiplus();
    static void releaseGdiplus();

    // 离屏渲染: 把绘制任务回放到 image 自身的像素缓冲 (在其上做 SourceOver 合成,
    // 保留原图内容, 与 QPainter 作用于 QImage 语义一致)。坐标系为 image 像素
    // (scale = 1, 逻辑单位 == 像素)。返回 false 表示 image 无效或 GDI+ 不可用。
    // 典型用法见 JZPainter(JZImage&): 构造/绘制/析构自动完成合成。
    static bool renderLayerToImage(JZImage& image, JZLayer* layer, float scale = 1.0f);

private:
    bool createMemBitmap(unsigned physicalW, unsigned physicalH);
    void releaseMemBitmap();
    void replayTasks(void* graphics, JZLayer* layer);

    // JZImageTask 的懒转换缓存: JZImageData -> JZPixmap (见 JZPixmap::fromImage)。
    // 只想画一两次的图走这条路径就够了; 要反复画/自己掌握转换时机与生命周期的,
    // 用 JZPixmap::fromImage + JZPainter::drawPixmap (Pixmap 任务不查缓存)。
    // 缓存按 weak_ptr 判活, 源图销毁即回收 (一张全屏图几十 MB)。
    std::shared_ptr<JZPixmapData> imagePixmap(const std::shared_ptr<JZImageData>& img);
    void purgeImageCache();

    void* m_hwnd = nullptr;      // 目标窗口; 为 nullptr 表示离屏 (不提交上屏)
    void* m_memDC = nullptr;     // HDC (内存, 双缓冲)
    void* m_memBmp = nullptr;    // HBITMAP (DIB section)
    void* m_bits = nullptr;      // DIB 像素首地址 (top-down BGRA; snapshot 取帧用)
    void* m_oldBmp = nullptr;
    unsigned m_physW = 0;
    unsigned m_physH = 0;

    // init() 是否成功过。destroy() 必须与 init() 对称: 未成功 init 就 destroy 时
    // 不能减 GDI+ 的引用计数，否则会把别的窗口还在用的 GDI+ 提前 Shutdown。
    bool m_initialized = false;

    struct ImageEntry {
        std::weak_ptr<JZImageData> image;      // 判活 + 淘汰 (不持所有权, 不拖住源图)
        std::shared_ptr<JZPixmapData> pixmap;  // 转换好的显示用位图
    };
    std::vector<ImageEntry> m_images;          // 被绘制过的 JZImage 的 pixmap 缓存

    JZDisplayDevice* m_widgetDevice = nullptr;
};

#endif // JZ_GDI_BACKEND_H_
