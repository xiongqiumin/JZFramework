#ifndef JZ_WINDOW_H_
#define JZ_WINDOW_H_

#include "JZDisplayDevice.h"

// 顶层窗口 = 显示设备。
// JZDisplayDevice 承担设备职责 (原生窗口/渲染后端/事件分发), JZWindow 在其上
// 提供常规窗口语义 (标题等), 是应用的常规入口:
//
//     JZApplication app;
//     JZWindow win(800, 600, "Demo");
//     MainWindow content(&win);   // 内容控件以窗口为父级
//     content.show();
//     app.exec();
//
// 窗口创建是显式的 (构造即创建), 不在 show() 中隐式创建:
// 创建可失败需要错误通道、布局需在构造期得知设备几何、原生窗口须在主线程创建。
// 离屏/多屏等高级用法直接使用 JZDisplayDevice。
class JZWindow : public JZDisplayDevice
{
    JZ_OBJECT_DECLARE(JZWindow)

public:
    JZWindow(int w, int h, const std::string& title = "JZWindow");
    ~JZWindow();
};

#endif // JZ_WINDOW_H_
