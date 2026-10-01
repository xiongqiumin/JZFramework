#ifndef JZ_VIRTUAL_SCREEN_VIEW_H_
#define JZ_VIRTUAL_SCREEN_VIEW_H_
#include "JZWidget.h"

class JZVirtualDevice;

// ==================== 虚拟桌面的"显示器"控件 ====================
// 把 JZVirtualDevice 的合成图 (整块虚拟屏) 画出来, 并把**用户的**鼠标/键盘换算成桌面坐标
// 投给这块桌面 —— "用户点宿主的客户区 -> 做转换 -> 发给窗口"这条路只在这里实现一次。
//
// 尺寸: 控件按**逻辑**尺寸 (desktopSize) 布局, 即"虚拟屏 800x600"; 合成图是**物理像素**
// (desktopSize * 宿主设备缩放, 见 JZVirtualDevice::scale), 画的时候按逻辑尺寸铺进去,
// 正好被宿主的世界变换 1:1 映射到屏幕像素 —— 与主机共享 DPI, 不重采样 (文字清晰)。
// 正因为缩放同步给了桌面, 事件换算只用逻辑坐标: 视图坐标 == 桌面坐标。
//
// 宿主窗口不够大时由外层 JZScrollArea 滚动; 拖动窗口时视图会抓取鼠标, 于是鼠标冲出视图
// 也不丢 move/up。
class JZVirtualScreenView : public JZWidget
{
public:
    explicit JZVirtualScreenView(JZVirtualDevice* screen = nullptr, JZWidget* parent = nullptr);
    ~JZVirtualScreenView() override;

    void setScreen(JZVirtualDevice* screen);
    JZVirtualDevice* screen() const { return m_screen; }

    JZSize sizeHint() const override;

    // 立即重绘 (同步 paintEvent + 上屏): 拖动/缩放窗口**进行中**由桌面调 (见
    // JZVirtualDevice::refreshNow)。常规 update() 只排一个脏区, 要等消息循环下一轮 ——
    // 快速拖动时那就是"窗口慢半拍"。
    void paintNow();

protected:
    void paintEvent(JZPainterEvent* event) override;
    void mousePressEvent(JZMouseEvent* event) override;
    void mouseReleaseEvent(JZMouseEvent* event) override;
    void mouseMoveEvent(JZMouseEvent* event) override;
    void wheelEvent(JZMouseEvent* event) override;
    void keyPressEvent(JZKeyEvent* event) override;
    void keyReleaseEvent(JZKeyEvent* event) override;

private:
    // 视图坐标 -> 桌面坐标 (1:1 显示时就是同一个数, 换算只在这一处)
    JZPoint toScreen(const JZPoint& local) const;
    // 与主机共享 DPI: 把本控件所在窗口的设备缩放同步给桌面 (桌面按它渲染物理帧缓冲)。
    // 宿主 DPI 会变 (WM_DPICHANGED), 所以每次绘制前都对一下 —— 两个 float 比较, 可忽略。
    void syncHostScale();

    JZVirtualDevice* m_screen = nullptr;
};

#endif // JZ_VIRTUAL_SCREEN_VIEW_H_
