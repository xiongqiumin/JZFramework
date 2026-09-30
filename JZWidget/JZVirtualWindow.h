#ifndef JZ_VIRTUAL_WINDOW_H_
#define JZ_VIRTUAL_WINDOW_H_
#include "JZWindow.h"
#include "JZImage.h"

class JZVirtualDevice;

// ==================== 虚拟桌面上的一个顶层窗口 (无原生窗口) ====================
// 与真实顶层窗口**同构**: 有自己的客户区、渲染设备、焦点/抓取/输入蒙版, 也有
// **屏幕坐标** (screenX/screenY, 桌面左上角为原点) —— 所以 clientToScreen /
// screenToClient / z 序 / "点击落在哪个窗口" 这些语义全部成立。
//
// 与真实窗口的唯一区别: 不创建平台窗口 (showEvent 不 createWindow) ——
//   - 像素画进自己的离屏帧缓冲 (JZDisplayDevice::initOffscreen, scale 恒为 1);
//   - 输入由 JZVirtualDevice 从"桌面屏幕坐标"路由进来 (先换算成客户区坐标);
//   - IME 无需实现 (JZWindow 的 IME 入口本就按 !m_hwnd 短路)。
//
// 归属: 由 JZVirtualDevice 创建并持有; **不登记**进 JZApplication 的顶层窗口表,
// 也不参与宿主 app 的"活动窗口"判定 (都见 JZWindow::SkipTopLevelRegistration /
// registerAsAppActiveWindow)。
class JZFRAMEWORK_API JZVirtualWindow : public JZWindow
{
public:
    JZVirtualWindow(JZVirtualDevice* desktop, int w, int h,
                    const std::string& title = "Window");
    ~JZVirtualWindow() override;

    JZVirtualDevice* desktop() const { return m_desktop; }

    // ---- 桌面坐标 (屏幕逻辑坐标; 桌面左上角为原点; 虚拟态 scale 恒为 1) ----
    int screenX() const { return m_screenX; }
    int screenY() const { return m_screenY; }
    JZRect screenGeometry() const { return JZRect(m_screenX, m_screenY, width(), height()); }
    void setScreenPos(int x, int y);

    // 首次 show: 建离屏帧缓冲 (不建原生窗口); 之后每次都是"重绘 + 通知桌面合成"。
    void showEvent(JZShowEvent* event) override;
    void resize(int w, int h) override;

    // 重绘本窗口一帧 (并通知桌面刷新合成图); 返回是否真的画了
    bool renderNow();
    // 本窗口客户区的一帧 (RGBA8888, 尺寸 = 逻辑尺寸); renderFirst 默认先重绘
    JZImage snapshot(bool renderFirst = true);

    // ---- 输入注入 (坐标 = **本窗口客户区逻辑坐标**; 桌面路由时已换算好) ----
    bool injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                     int x, int y, int wheelDelta = 0,
                     uint32_t modifiers = JZKeyEvent::NoModifier);
    bool injectClick(int x, int y, JZMouseEvent::Button button = JZMouseEvent::LeftButton);
    bool injectKey(uint32_t key, const std::string& text = std::string(),
                   uint32_t modifiers = JZKeyEvent::NoModifier, bool press = true);
    // 键盘焦点 (由桌面在切换活动窗口时调用; 等价 WM_SETFOCUS / WM_KILLFOCUS)
    void injectFocus(bool got);
    bool isActive() const { return m_hasWindowFocus; }

protected:
    // 不登记为宿主 app 的"活动窗口" (虚拟桌面的活动窗口由桌面管理器维护)
    bool registerAsAppActiveWindow() const override { return false; }

    JZVirtualDevice* m_desktop = nullptr;
    bool m_offscreenReady = false;    // 离屏帧缓冲是否已建
};

#endif // JZ_VIRTUAL_WINDOW_H_
