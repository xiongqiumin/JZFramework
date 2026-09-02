#ifndef JZ_WINDOW_H_
#define JZ_WINDOW_H_

#include "JZWidget.h"
#include "JZDisplayDevice.h"

// 顶层窗口 = 原生窗口宿主 + 根控件。
//   - 组合一个 JZDisplayDevice (渲染调度), 自身作为渲染树根;
//   - 拥有原生 Win32 窗口的创建/销毁、WndProc 消息循环、显隐、DPI 响应;
//     WndProc 把鼠标/键盘消息翻译成 JZEvent 后经 onWindowProcess 分发到控件树
//     (ImGui 只负责绘图);
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
    // 窗口形态 (惰性创建, show() 前设置):
    //   Normal — 常规顶层窗口 (可缩放);  Popup — 无边框浮层 (不进任务栏,
    //   鼠标抓取: 点外部/Esc 自动关闭, 供下拉等使用);  Modal — 对话框框形
    //   (标题栏+关闭, 无最小/最大化), 配合 execModal() 使用。
    enum WindowType { Normal, Popup, Modal };
    void setWindowType(WindowType t) { m_windowType = t; }
    WindowType windowType() const { return m_windowType; }

    JZWindow(JZWidget* parent = nullptr);   // 默认尺寸 800x600
    JZWindow(int w, int h, const std::string& title = "JZWindow", JZWidget* parent = nullptr);
    ~JZWindow();

    void setTitle(const std::string& title);
    std::string title() const;

    JZDisplayDevice* device();         // 渲染设备
    void doPaint(const JZRect& rc);    // 重绘指定区域 (paint task 执行)
    virtual void resize(int w, int h) override;        // 同步原生窗口尺寸

    // 置顶; 最小化/最大化/还原
    void setTop(bool on);
    void showMinimized();
    void showMaximized();
    void showNormal();

    // 屏幕逻辑坐标定位 (popup 等自定位窗口; 物理像素换算在内部)。
    // 惰性: 窗口未创建时只记录位置, createWindow 时作为原生窗口初始位置;
    // 显式定位会关闭显示自动居中 (等价 Qt 的 WA_Moved)。
    void moveScreen(int x, int y);

    // 显示时自动定位 (Modal/Popup 默认开): 未显式 moveScreen 定位时相对
    // owner 窗口居中 (无 owner 则 CW_USEDEFAULT)。Normal 窗口不受影响。
    void setAutoPosition(bool on) { m_autoPosition = on; }
    bool isAutoPosition() const { return m_autoPosition; }

    // 通用模态循环: 可见期间泵 Win32 消息 + 处理框架事件 + 绘制脏区;
    // 禁用/恢复 owner (disableOwner=false 时不禁用, 供下拉弹层用)。hide() 即退出。
    // 循环体在 JZWin32Thread (与主循环共享 processAndPaint 轮体), 此处仅转发。
    int execModal(bool disableOwner = true);

    // 原生窗口句柄: Win32 下即 HWND, 窗口未创建 (惰性) 时返回 nullptr。
    // 文本控件要用它调用 IMM 接口把光标位置回写给输入法。
    void* nativeHandle() const { return m_hwnd; }

    // ---- AI 语义 ----
    std::string aiRole() const override { return "window"; }
    std::string aiName() const override { return m_title; }

    // ---- 焦点 (窗口级: 同 HWND 内多控件共享, 由本窗口持有当前焦点控件) ----
    JZWidget* focusWidget() const { return m_focusWidget; }
    void setFocusWidget(JZWidget* w);   // w==nullptr 等价清空焦点

    // ---- 输入法 (IME) 输入上下文 ----
    // 整个窗口只有一个 HWND / 一个输入上下文 (IMC), 故 IMC 的关联/断开必须在
    // 窗口级统一做: 窗口有焦点 **且** 焦点控件 imeEnabled() 时才关联, 否则
    // ImmAssociateContext(NULL) 断开 (断开后 ImmGetContext 返回 NULL, 本窗口
    // 不再产生任何 IME 消息)。控件级别不要各自去关联/断开。
    void updateImeContext();

    // 让 IME 的组字窗/候选窗使用与本窗口一致的字体, 并保证输入法处于打开状态。
    // 由文本控件在 focusInEvent 里调用, fontPixelSize 为其正文字号 (逻辑像素)。
    void prepareIme(int fontPixelSize);

    // 本次组字是否已通过 GCS_RESULTSTR 上屏 (用于 WM_IME_CHAR 去重)
    bool imeGotResult() const { return m_imeGotResult; }
    void setImeGotResult(bool v) { m_imeGotResult = v; }

    // 事件处理统一入口: WndProc 把 Win32 消息翻译成 JZEvent 后调到这里,
    // 按类型分发 (鼠标 -> 命中测试, 键盘 -> 从焦点逐级上传, IME -> 焦点控件)。
    // 返回 true 表示事件已被处理 (WndProc 不再转交 DefWindowProc)。
    bool onWindowProcess(JZEvent* event);

    // 事件入口: invalidate 在此合成到主线程 paint task (不依赖队列扫描),
    // 其余交基类按类型分发。
    bool event(JZEvent* event) override;

    // ---- 由 WndProc / 对话框调用 ----
    void windowResized(int logicalW, int logicalH);   // 原生窗口尺寸变化 (WM_SIZE)
    void dpiChanged();                                 // 显示器 DPI 变化 (WM_DPICHANGED)

    // WM_CLOSE 入口: 触发框架关闭流程 closeEvent, 重写方 accept/ignore 表态,
    // 这里统一执行关闭动作。返回是否真的关闭。
    bool requestClose();
    // 原生窗口已销毁 (WM_DESTROY): 清宿主状态避免句柄悬空, 之后 show() 可重建。
    void nativeDestroyed();

protected:
    // 显隐: show() 首次触发 showEvent -> 惰性创建原生窗口并显示;
    // hide() 触发 hideEvent -> 隐藏原生窗口 (未创建则为 no-op)。
    virtual void showEvent(JZShowEvent* event) override;
    virtual void hideEvent(JZHideEvent* event) override;
    virtual void paintEvent(JZPainterEvent* event) override;
    virtual void keyPressEvent(JZKeyEvent* event) override;

    // 命中测试: 返回包含全局逻辑坐标的最深层可见控件 (不含子窗口/根自身)
    JZWidget* hitTest(const JZPoint& global);
    JZWidget* hitTestRec(JZWidget* w, const JZPoint& global);

    // 输入分发 (onWindowProcess 按事件类型调用)
    bool dispatchMouse(JZMouseEvent* ev);   // 命中测试定位控件, 回填 localPos 后投递
    bool dispatchKey(JZKeyEvent* ev);       // 键盘: 从焦点控件起, 未处理的键逐级向上传播
    bool dispatchIme(JZImeEvent* ev);       // IME: 投递给焦点控件; 无焦点控件返回 false
    void onWindowFocus(bool got);           // 窗口获/失焦: 管理 IMC + 派发焦点进/出事件

protected:
    void createWindow();

    JZDisplayDevice* m_device = nullptr;
    void* m_hwnd = nullptr;             // HWND (头文件零 Win32 类型)
    bool m_windowCreated = false;       // 原生窗口是否已创建 (惰性, showEvent 触发)
    std::string m_title;
    WindowType m_windowType = Normal;
    bool m_top = false;                 // 置顶

    JZWidget* m_focusWidget = nullptr;  // 当前焦点控件 (接收键盘/IME)
    void* m_imeContext = nullptr;       // 窗口级 IMC 句柄 (首次需要时取得并缓存)
    bool m_hasWindowFocus = false;      // 原生窗口是否有键盘焦点
    bool m_imeGotResult = false;        // 本次组字是否已由 ResultStr 上屏

    // 显示定位: 显式 moveScreen 记录 (等价 Qt WA_Moved) 优先于自动居中
    bool m_autoPosition = true;         // 显示时相对 owner 居中 (Modal/Popup)
    bool m_hasScreenPos = false;        // 用户显式 moveScreen 过
    int  m_screenX = 0;                 // 记录的屏幕逻辑坐标 (窗口未创建时暂存)
    int  m_screenY = 0;
};

#endif // JZ_WINDOW_H_

