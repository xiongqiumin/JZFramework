#ifndef JZ_VIRTUAL_DEVICE_H_
#define JZ_VIRTUAL_DEVICE_H_
#include "JZWindow.h"
#include "JZImage.h"

class JZImageLabel;

// ==================== 虚拟设备 (无原生窗口) ====================
// 承载一棵控件树, 但**不创建任何平台窗口**:
//   - 渲染: 走离屏帧缓冲 (JZDisplayDevice::initOffscreen + 默认 GDI 后端),
//     与真实窗口共用同一套绘制路径 (控件 paintEvent -> JZLayer -> 后端回放);
//   - 输入: 由程序注入 (injectXxx), 与真实输入**同一条分发路径** (onWindowProcess),
//     因此 hitTest / 焦点 / 事件过滤器 / 输入蒙版语义完全一致;
//   - 消息循环不动: 脏区绘制仍走 jzApp 的泵 -> JZWindow::doPaint (本类只把
//     m_windowCreated 置真), 所以"虚拟模式"不影响 Win32 循环, 也不需要第二套循环;
//   - IME: JZWindow 的 IME 入口在无 HWND 时全部自行短路 (见 updateImeContext /
//     updateImeUi / cancelImeComposition / prepareIme), 故虚拟态无需实现 IME;
//   - 取帧: snapshot() 直接读帧缓冲, 即"窗口上会看到的那一帧", 不受遮挡/z 序/
//     前台/多屏影响, 也不需要抓屏 (JZScreen::grabWindow 在离屏下拿不到内容);
//   - 可选查看器 (showViewer): 一个普通 JZWindow 把每帧显示出来供人观察。
//     它只读帧, 不参与本设备的输入/焦点/时钟; 关掉它不影响本设备的处理。
//
// 用法 (computer-use / 自动化回放):
//   auto* dev = new JZVirtualDevice(800, 600);
//   page->setParentWidget(dev);
//   dev->show();                            // 只建离屏设备, 不建窗口
//   dev->injectClick(120, 80);              // 逻辑客户区坐标
//   dev->injectKey(JZKey::Key_Return);
//   JZImage frame = dev->snapshot();        // 取一帧 (会先把当前状态画出来)
class JZFRAMEWORK_API JZVirtualDevice : public JZWindow
{
public:
    JZVirtualDevice(int w, int h, const std::string& title = "VirtualDevice",
                    JZWidget* parent = nullptr);
    ~JZVirtualDevice() override;

    // 首次 show: 建离屏帧缓冲 + 铺开首帧 (不建原生窗口)。hide(): 只改框架层可见性。
    void showEvent(JZShowEvent* event) override;
    void resize(int w, int h) override;

    // ---- 渲染 / 取帧 ----
    // 立刻重绘一帧 (与真实窗口下的重绘同一路径)。返回是否真的画了。
    bool renderNow();
    // 取当前帧 (RGBA8888; 物理像素 = 逻辑像素, 虚拟态 scale 恒为 1)。
    // 默认先 renderNow() 保证是"当前状态"的帧; renderFirst=false 则只读现有帧缓冲
    // (查看器刷新等已在帧内的场合用, 避免重入)。
    JZImage snapshot(bool renderFirst = true);

    // ---- 程序化输入注入 ----
    // 坐标 = 客户区逻辑坐标 (虚拟设备原点即 (0,0))。
    // 事件标记为 programmatic: 与 AI 合成点击同义, 不受输入蒙版屏蔽。
    bool injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                     int x, int y, int wheelDelta = 0,
                     uint32_t modifiers = JZKeyEvent::NoModifier);
    // 点击 = 按下 + 抬起 (最常用; 需要拖拽时用 injectMouse 自己发 Move 序列)
    bool injectClick(int x, int y, JZMouseEvent::Button button = JZMouseEvent::LeftButton);
    // 键盘: press=false 表示抬起。text 为按键产生的可打印字符 (UTF-8), 功能键留空。
    bool injectKey(uint32_t key, const std::string& text = std::string(),
                   uint32_t modifiers = JZKeyEvent::NoModifier, bool press = true);
    // 窗口获得/失去键盘焦点 (等价 WM_SETFOCUS / WM_KILLFOCUS; 无 Win32 参与)
    void injectFocus(bool got);

    // ---- 可选查看器 (供人查看; 不参与本设备输入/焦点/时钟) ----
    void showViewer(const std::string& title = "Virtual Viewer");
    void hideViewer();
    bool hasViewer() const { return m_viewer != nullptr; }
    // 把当前帧推给查看器 (每帧收尾自动调用; 无查看器时 no-op)
    void updateViewer();

protected:
    void afterFrame();                   // 一帧产出后的收尾 (推送查看器)

    JZWindow* m_viewer = nullptr;        // 查看窗口 (普通窗口, 独立于本设备)
    JZImageLabel* m_viewerLabel = nullptr;
    bool m_offscreenReady = false;       // 离屏帧缓冲是否已建
};

#endif // JZ_VIRTUAL_DEVICE_H_
