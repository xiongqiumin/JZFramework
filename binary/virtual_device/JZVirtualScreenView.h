#ifndef JZ_VIRTUAL_SCREEN_VIEW_H_
#define JZ_VIRTUAL_SCREEN_VIEW_H_
#include "JZWidget.h"

class JZVirtualDevice;

// ==================== 虚拟桌面的"显示器"控件 ====================
// 把 JZVirtualDevice 的合成图 (整块虚拟屏) 画出来, 并把**用户的**鼠标/键盘换算成桌面坐标
// 投给这块桌面 —— "用户点宿主的客户区 -> 做转换 -> 发给窗口"这条路只在这里实现一次。
//
// 1:1 显示 (虚拟屏 800x600 就是 800x600 像素; 视图坐标 == 桌面坐标)。宿主窗口不够大时
// 由外层 JZScrollArea 滚动; 拖动窗口时视图会抓取鼠标, 于是鼠标冲出视图也不丢 move/up。
class JZVirtualScreenView : public JZWidget
{
public:
    explicit JZVirtualScreenView(JZVirtualDevice* screen = nullptr, JZWidget* parent = nullptr);
    ~JZVirtualScreenView() override;

    void setScreen(JZVirtualDevice* screen);
    JZVirtualDevice* screen() const { return m_screen; }

    JZSize sizeHint() const override;

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

    JZVirtualDevice* m_screen = nullptr;
};

#endif // JZ_VIRTUAL_SCREEN_VIEW_H_
