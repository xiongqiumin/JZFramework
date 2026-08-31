#ifndef JZ_IMGUI_BACKEND_H_
#define JZ_IMGUI_BACKEND_H_

#include <map>

// DX / ImGui 类型前向声明 (完整类型仅 JZImGuiBackend.cpp 需要)
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ImDrawList;
struct ImFont;

class JZDisplayDevice;
class JZLayer;
class JZImageData;

// ==================== ImGui 渲染后端 (纯绘图) ====================
// 只负责一件事: 把 JZLayer 的 JZDrawTask 列表画出来并 Present。
//   - init(): DX11 设备/交换链 + ImGui 上下文 (hwnd 由 JZDisplayDevice
//     创建并所有, 这里仅作为渲染目标);
//   - render(): NewFrame -> 任务回放到 GetBackgroundDrawList() -> Render
//     -> OMSetRenderTargets/Clear -> RenderDrawData -> Present;
//   - 不创建窗口、不处理消息、不消费任何输入 (窗口与消息在 JZDisplayDevice)。
//
// 坐标系: 绘制任务为逻辑像素, 回放时 * device->scale() -> 物理像素。
class JZImGuiBackend
{
public:
    JZImGuiBackend();
    ~JZImGuiBackend();

    // 初始化 DX11 + ImGui。hwnd/尺寸由 JZDisplayDevice 提供。
    bool init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH);
    void destroy();

    // 渲染一帧: 回放 layer 中的绘制任务并 Present, 结束后清空任务列表。
    void render(JZLayer* layer);

    // 原生窗口尺寸变化 (JZDisplayDevice 的 WM_SIZE 分支调用)。
    void resizeSwapChain(unsigned physicalW, unsigned physicalH);

    // DPI 缩放变化后重建字体 (JZDisplayDevice 调用)。
    void rebuildFonts();

private:
#ifdef _WIN32
    bool createD3DDevice(void* hwnd, unsigned physicalW, unsigned physicalH);
    void createRenderTarget();
    void releaseRenderTarget();
    void replayTasks(ImDrawList* dl, JZLayer* layer);
    void* imageTexture(const JZImageData* img);   // 懒上传, 返回 ID3D11ShaderResourceView*

    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;

    ImFont* m_font = nullptr;                            // 动态字体: 任意尺寸绘制
    std::map<const JZImageData*, void*> m_textures;      // JZImageData -> SRV
#else
    void* m_unused = nullptr;
#endif

    JZDisplayDevice* m_widgetDevice = nullptr;
};

#endif // JZ_IMGUI_BACKEND_H_
