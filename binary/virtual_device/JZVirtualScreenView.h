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

    // ---- 输入法: 虚拟侧填给宿主的两样东西 (都由 runOnView 在本控件的线程上调) ----
    // 组字窗矩形 / 候选窗锚点, **桌面坐标** (虚拟侧的文本控件就是这么算的 —— 它自己的
    // mapToGlobal 落在桌面坐标系里)。宿主应答 WM_IME_REQUEST 时由本类换算成宿主屏幕坐标。
    void setImeWindowPos(const JZRect& lineRect, const JZPoint& candPos);

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
    void imeEvent(JZImeEvent* event) override;

    // 这块"显示器"自己不接输入法, 但它替桌面上**真正该组字的那个控件**举手:
    // 返回 true 时宿主窗口 (真 HWND) 才会给 IME 打开组字窗/候选窗, 之后的 WM_IME_*
    // 经上面的 imeEvent 转投到虚拟桌面 (少了这一步, 虚拟桌面里打字输入法是被关着的 ——
    // 表现为"选中输入框也没反应")。
    bool imeEnabled() const override;
    bool imeWindowRect(JZRect& lineRect, JZPoint& candPos) const override;

private:
    // 视图坐标 -> 桌面坐标 (1:1 显示时就是同一个数, 换算只在这一处)
    JZPoint toScreen(const JZPoint& local) const;
    // 与主机共享 DPI: 把本控件所在窗口的设备缩放同步给桌面 (桌面按它渲染物理帧缓冲)。
    // 宿主 DPI 会变 (WM_DPICHANGED), 所以每次绘制前都对一下 —— 两个 float 比较, 可忽略。
    void syncHostScale();

    JZVirtualDevice* m_screen = nullptr;
    // 组字/候选窗的位置 (**桌面坐标**; 由虚拟侧的文本控件算出并转过来) 与"有没有这份位置"
    JZRect m_imeLine;
    JZPoint m_imeCand;
    bool m_imeHasPos = false;
};

#endif // JZ_VIRTUAL_SCREEN_VIEW_H_
