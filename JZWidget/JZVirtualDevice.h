#ifndef JZ_VIRTUAL_DEVICE_H_
#define JZ_VIRTUAL_DEVICE_H_
#include "JZWindow.h"
#include "JZImage.h"
#include <string>
#include <vector>

class JZVirtualWindow;
class JZImageLabel;

// ==================== 虚拟桌面 (多窗口管理器 + 宿主窗口) ====================
// 它本身是一个**真窗口** (可常驻显示, 供人查看), 里面装着一块虚拟桌面:
//   - 桌面白屏: m_desktop 是一张 desktopW × desktopH 的图 (逻辑单位 == 像素, scale=1);
//   - 多窗口: 管理若干 JZVirtualWindow (无原生窗口的顶层窗口), 各自有桌面坐标与 z 序;
//   - 坐标有含义: 桌面坐标 = 屏幕逻辑坐标 => screenToClient / clientToScreen /
//     "点击落在哪个窗口" / 窗口归一化 rect (相对整屏) 全部成立, 与真实桌面同构;
//   - 合成: 桌面白屏 + 各窗口的帧 (按 z 序由下到上) => m_desktop;
//   - 取帧: screenshot() 整屏 / screenshotWindow(w) 单窗口 —— 直接读帧缓冲, 不抓屏;
//   - 注入: 给**桌面屏幕坐标**, 自动命中窗口 + 激活 + 换算成客户区坐标后投递。
//
// 与宿主 app 的关系: 桌面内部的窗口不登记进 JZApplication 的顶层窗口表, 也不参与其
// "活动窗口" 判定 (见 JZWindow::SkipTopLevelRegistration / registerAsAppActiveWindow),
// 所以"虚拟桌面在跑"不会干扰宿主 app 的窗口生命周期判定。
//
// 显示是可选的: show() 把桌面显示出来给人看 (宿主窗口); **不 show 也能完整工作**
// (无 HWND, 适合 ctest / CI / 纯无头回放)。
class JZFRAMEWORK_API JZVirtualDevice : public JZWindow
{
public:
    JZVirtualDevice(int desktopW, int desktopH,
                    const std::string& title = "Virtual Desktop");
    ~JZVirtualDevice() override;

    int desktopWidth() const { return m_desktopW; }
    int desktopHeight() const { return m_desktopH; }

    // 桌面底色 (默认**白屏**)。换成灰之类便于肉眼区分"桌面"与"窗口" —— 两者都是白的话
    // 窗口在整屏帧里看不出来。只影响底色, 不影响任何坐标/语义。
    void setDesktopBackground(const JZColor& c);
    const JZColor& desktopBackground() const { return m_desktopBg; }

    // ---- 多窗口管理 ----
    // 建窗口 (默认居中并按已有窗口数级联偏移, 避免完全叠住); 新窗口置顶并激活。
    JZVirtualWindow* createWindow(int w, int h, const std::string& title = "Window");
    JZVirtualWindow* createWindow(int w, int h, const std::string& title, int x, int y);
    // 关掉 (隐藏并销毁) 一个窗口
    void destroyWindow(JZVirtualWindow* w);

    // 窗口列表 (z 序: 由下到上, 末尾最上)
    const std::vector<JZVirtualWindow*>& windows() const { return m_windows; }
    JZVirtualWindow* activeWindow() const { return m_active; }
    void raise(JZVirtualWindow* w);            // 置顶 (只改 z 序)
    void activate(JZVirtualWindow* w);          // 置顶 + 键盘焦点 (切换活动窗口)
    void moveWindow(JZVirtualWindow* w, int x, int y);
    // 按标题片段找窗口 (等价 CU 的 titleFilter)
    JZVirtualWindow* byTitle(const std::string& title) const;
    // 桌面坐标命中: z 序自上而下第一个包含该点的**可见**窗口; 无则 nullptr
    JZVirtualWindow* windowAt(int screenX, int screenY) const;

    // ---- 坐标换算 (屏幕逻辑坐标 <-> 窗口客户区逻辑坐标) ----
    JZPoint screenToClient(JZVirtualWindow* w, int screenX, int screenY) const;
    JZPoint clientToScreen(JZVirtualWindow* w, int clientX, int clientY) const;

    // ---- 合成 / 取帧 ----
    const JZImage& desktopImage() const { return m_desktop; }
    // 桌面白屏 + 各窗口 (按 z 序) -> m_desktop。合成中不重绘窗口 (用它们现有帧缓冲)。
    void compose();
    // compose + 推给宿主窗口里的显示控件
    void refresh();
    JZImage screenshot();                      // 整屏一帧
    JZImage screenshotWindow(JZVirtualWindow* w);

    // ---- 输入注入 (坐标 = 桌面屏幕逻辑坐标) ----
    // 命中窗口 -> 激活它 -> 换算成该窗口客户区坐标 -> 投递 (与真实桌面点击同语义)
    bool injectMouseAtScreen(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                             int screenX, int screenY, int wheelDelta = 0,
                             uint32_t modifiers = JZKeyEvent::NoModifier);
    bool injectClickAt(int screenX, int screenY,
                       JZMouseEvent::Button button = JZMouseEvent::LeftButton);
    // 键盘发给当前活动窗口
    bool injectKey(uint32_t key, const std::string& text = std::string(),
                   uint32_t modifiers = JZKeyEvent::NoModifier, bool press = true);
    // 按 UTF-8 逐字符输入 (可打印字符; 组合键/IME 走 injectKey / ImeEvent)
    bool injectText(const std::string& utf8);

    // 宿主窗口 (可选): show() 显示虚拟桌面供人查看; 不 show 也能跑 (无 HWND)
    void showEvent(JZShowEvent* event) override;
    void resize(int w, int h) override;

    // ---- 内部: 由 JZVirtualWindow 在自身帧/位置/状态变化时调用 ----
    void onWindowFrameChanged(JZVirtualWindow* w);

protected:
    JZImageLabel* ensureView();                // 惰性建宿主窗口里的显示控件

    std::vector<JZVirtualWindow*> m_windows;   // z 序: 末尾最上
    JZVirtualWindow* m_active = nullptr;       // 当前活动窗口 (持键盘焦点)
    JZImage m_desktop;                         // 桌面底色 + 窗口合成图
    JZColor m_desktopBg = JZColor(255, 255, 255, 255);   // 桌面底色 (默认白屏)
    JZImageLabel* m_view = nullptr;            // 宿主窗口内显示桌面的控件 (可选)
    int m_desktopW = 0;
    int m_desktopH = 0;
    bool m_composing = false;                  // 合成中: 忽略窗口的刷新通知 (防重入)
};

#endif // JZ_VIRTUAL_DEVICE_H_
