#ifndef JZ_WINDOW_H_
#define JZ_WINDOW_H_

#include "JZWidget.h"
#include "JZDisplayDevice.h"

// 顶层窗口 = 原生窗口宿主 + 根控件。
//   - 组合一个 JZDisplayDevice (渲染调度), 自身作为渲染树根;
//   - 拥有原生 Win32 窗口的创建/销毁、WndProc 消息循环、显隐、DPI 响应;
//     鼠标/键盘消息经 逻辑像素 换算后交给设备分发到控件树 (ImGui 只负责绘图);
//   - 窗口惰性创建: 构造零系统资源, 首次 show() (showEvent) 才创建并显示。
//
// 用法:
//     JZApplication app;
//     JZWindow win(800, 600, "Demo");   // 或 MainWindow win; (继承 JZWindow)
//     MainWindow content(&win);          // 内容控件以窗口为父级
//     content.show();
//     app.exec();
//
// 离屏渲染等高级用法直接使用 JZDisplayDevice (setRootWidget + backend 手动驱动)。
class JZWindow : public JZWidget
{
    JZ_OBJECT_DECLARE(JZWindow)

public:
    JZWindow(JZWidget* parent = nullptr);   // 默认尺寸 800x600
    JZWindow(int w, int h, const std::string& title = "JZWindow", JZWidget* parent = nullptr);
    ~JZWindow();

    void setTitle(const std::string& title);
    std::string title() const;

    JZDisplayDevice* device();         // 渲染设备
    void doPaint(const JZRect& rc);    // 重绘指定区域 (paint task 执行)
    virtual void resize(int w, int h) override;        // 同步原生窗口尺寸

    // ---- 由 WndProc / 对话框调用 ----
    void windowResized(int logicalW, int logicalH);   // 原生窗口尺寸变化 (WM_SIZE)
    void dpiChanged();                                 // 显示器 DPI 变化 (WM_DPICHANGED)

protected:
    // 显隐: show() 首次触发 showEvent -> 惰性创建原生窗口并显示;
    // hide() 触发 hideEvent -> 隐藏原生窗口 (未创建则为 no-op)。
    virtual void showEvent(JZShowEvent* event) override;
    virtual void hideEvent(JZHideEvent* event) override;
    virtual void paintEvent(JZPainterEvent* event) override;

protected:
    void createWindow();

    JZDisplayDevice* m_device = nullptr;
    void* m_hwnd = nullptr;             // HWND (头文件零 Win32 类型)
    bool m_windowCreated = false;       // 原生窗口是否已创建 (惰性, showEvent 触发)
    std::string m_title;
};

#endif // JZ_WINDOW_H_

