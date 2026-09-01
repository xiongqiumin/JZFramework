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

    // 原生窗口句柄: Win32 下即 HWND, 窗口未创建 (惰性) 时返回 nullptr。
    // 文本控件要用它调用 IMM 接口把光标位置回写给输入法。
    void* nativeHandle() const { return m_hwnd; }

    // ---- 焦点 (窗口级: 同 HWND 内多控件共享, 由本窗口持有当前焦点控件) ----
    JZWidget* focusWidget() const { return m_focusWidget; }
    void setFocusWidget(JZWidget* w);   // w==nullptr 等价清空焦点

    // 输入分发 (由 WndProc 调用, 命中测试后投递到控件 / 转发给焦点控件)
    bool dispatchMouse(JZMouseEvent::MouseType t, JZMouseEvent::Button b,
                       const JZPoint& global, int wheelDelta);
    bool dispatchKey(uint32_t key, bool press, bool repeat);
    bool dispatchChar(const std::string& text);
    void postIme(JZImeEvent* ev);
    void onWindowFocus(bool got);   // WM_SETFOCUS/WM_KILLFOCUS: 管理 IMC + 派发焦点事件

    // ---- 由 WndProc / 对话框调用 ----
    void windowResized(int logicalW, int logicalH);   // 原生窗口尺寸变化 (WM_SIZE)
    void dpiChanged();                                 // 显示器 DPI 变化 (WM_DPICHANGED)

protected:
    // 显隐: show() 首次触发 showEvent -> 惰性创建原生窗口并显示;
    // hide() 触发 hideEvent -> 隐藏原生窗口 (未创建则为 no-op)。
    virtual void showEvent(JZShowEvent* event) override;
    virtual void hideEvent(JZHideEvent* event) override;
    virtual void paintEvent(JZPainterEvent* event) override;

    // 命中测试: 返回包含全局逻辑坐标的最深层可见控件 (不含子窗口/根自身)
    JZWidget* hitTest(const JZPoint& global);
    JZWidget* hitTestRec(JZWidget* w, const JZPoint& global);

protected:
    void createWindow();

    JZDisplayDevice* m_device = nullptr;
    void* m_hwnd = nullptr;             // HWND (头文件零 Win32 类型)
    bool m_windowCreated = false;       // 原生窗口是否已创建 (惰性, showEvent 触发)
    std::string m_title;

    JZWidget* m_focusWidget = nullptr;  // 当前焦点控件 (接收键盘/IME)
    void* m_imeContext = nullptr;       // 窗口级 IMC 句柄 (WM_SETFOCUS 时建立)
};

#endif // JZ_WINDOW_H_

