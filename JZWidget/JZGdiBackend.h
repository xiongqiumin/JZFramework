#ifndef JZ_GDI_BACKEND_H_
#define JZ_GDI_BACKEND_H_

#include <map>

class JZDisplayDevice;
class JZLayer;
class JZImageData;

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
class JZGdiBackend
{
public:
    JZGdiBackend();
    ~JZGdiBackend();

    // 绑定窗口 + 建双缓冲位图。hwnd/尺寸由 JZDisplayDevice 提供。
    bool init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH);
    void destroy();

    // 渲染一帧: 回放 layer 中的绘制任务并 BitBlt 到窗口, 结束后清空任务列表。
    void render(JZLayer* layer);

    // 原生窗口尺寸变化 (重建内存位图)。
    void resizeSwapChain(unsigned physicalW, unsigned physicalH);

    // 接口占位: GDI+ 字体按任务即时创建, DPI 变化无需重建。
    void rebuildFonts();

private:
    bool createMemBitmap(unsigned physicalW, unsigned physicalH);
    void releaseMemBitmap();
    void replayTasks(void* graphics, JZLayer* layer);
    void* imageBitmap(const JZImageData* img);   // 懒缓存 (RGBA8888 -> GDI+ 32bppARGB)

    void* m_hwnd = nullptr;
    void* m_memDC = nullptr;     // HDC (内存, 双缓冲)
    void* m_memBmp = nullptr;    // HBITMAP (DIB section)
    void* m_oldBmp = nullptr;
    unsigned m_physW = 0;
    unsigned m_physH = 0;

    std::map<const JZImageData*, void*> m_bitmaps;   // JZImageData -> Gdiplus::Bitmap*
    JZDisplayDevice* m_widgetDevice = nullptr;
};

#endif // JZ_GDI_BACKEND_H_
