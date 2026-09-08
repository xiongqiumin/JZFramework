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

    // 显式设定 DPI 缩放 (popup 等浮层跟随 owner 显示器, 多屏/HiDPI 下避免与主屏错配);
    // 一经设定, createWindow 不再用主屏 DPI 覆盖。
    void setDeviceScale(float scale);

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

    // 弹层(Popup)的"外部按下关闭"过滤, 由消息泵 JZWin32Thread::pumpOnce 在派发
    // 按下消息前调用。作用是替代 Popup 窗口各自的 SetCapture —— Win32 鼠标抓取
    // 是全局唯一的, 嵌套弹层(如菜单的子菜单)会互相抢走, 持有抓取的窗口之外
    // 再也收不到任何鼠标消息(悬停/选择全失效)。
    // 语义: 关闭所有"不含该屏幕点"的弹层(如点父菜单时只关子菜单);
    // 若该点不在任何弹层内, 返回 true 表示调用方应吞掉这条消息 —— 等价 Qt 菜单:
    // 外部点击只用于关闭弹层, 不再落到弹层下面的控件上。
    static bool filterPopupPress(int screenX, int screenY);

    // 原生窗口句柄: Win32 下即 HWND, 窗口未创建 (惰性) 时返回 nullptr。
    // 文本控件要用它调用 IMM 接口把光标位置回写给输入法。
    void* nativeHandle() const { return m_hwnd; }

    // ---- AI 语义 ----
    std::string aiRole() const override { return "window"; }
    std::string aiName() const override { return m_title; }

    // ---- 焦点 (窗口级: 同 HWND 内多控件共享, 由本窗口持有当前焦点控件) ----
    JZWidget* focusWidget() const { return m_focusWidget; }
    void setFocusWidget(JZWidget* w);   // w==nullptr 等价清空焦点

    // 鼠标抓取 (窗口级: 同 HWND 同时仅一个 grabber)。抓取后所有鼠标事件
    // (含客户区外) 直发 w, 直到 releaseMouse 或窗口隐藏。SetCapture 由本窗口托管。
    void grabMouse(JZWidget* w);            // w 成为 grabber 并 SetCapture(HWND)
    void releaseMouse(JZWidget* w = nullptr); // w==nullptr(或当前 grabber) 时解除抓取
    JZWidget* mouseGrabber() const { return m_mouseGrabber; }

    // 按当前光标位置取悬停控件 (grabber 优先) 的生效光标 JZWidget::cursor()
    // 并 SetCursor。WM_SETCURSOR (客户区内) 自动调用; setCursor 与鼠标派发后
    // 也会调用以立即刷新。
    void applyMouseCursor();

    // 悬停控件跟踪: 命中控件变化 (mouseMove / 光标离开客户区) 时, 给旧控件链派发
    // leaveEvent、给新控件链派发 enterEvent (含各自祖先, 止于最近公共祖先)。
    // 由 WndProc 的 WM_MOUSELEAVE 与 dispatchMouse 调用, 故为 public。
    void updateHover(JZWidget* newHover);

    // ---- 输入屏蔽 (全屏蒙版, AI 操作期间) ----
    // 设置 gate 后, gate 可见期间本窗口丢弃真实用户的鼠标/键盘/IME 输入, 只放行
    // gate 自身子树 (如蒙版上的"取消"按钮); Esc 会交给 gate 处理 (= 取消)。
    // gate==nullptr 解除屏蔽。AI 合成的程序化鼠标事件 (JZMouseEvent::setProgrammatic)
    // 不受屏蔽, 保证 AI 点击能穿过蒙版落到目标控件。
    void setInputGate(JZWidget* gate);
    JZWidget* inputGate() const { return m_inputGate; }

    // 是否活动窗口: 原生窗口持有键盘焦点 (由 WM_SETFOCUS / WM_KILLFOCUS 维护)。
    // JZApplication::activeWindow() 依据它判定当前活动窗口。
    bool isActiveWindow() const { return m_hasWindowFocus; }

    // ---- 输入法 (IME) 输入上下文 ----
    // 整个窗口只有一个 HWND / 一个输入上下文 (IMC), 故 IMC 的关联必须在窗口级
    // 统一做: 窗口创建后**始终关联**, 焦点是否接受输入法只影响 IME 的 UI 与组字
    // (见 imeActive())。
    //
    // 不要再按焦点频繁 ImmAssociateContext(NULL) 断开又关联: IMC 一旦断开重连,
    // 输入法会把该窗口的输入状态重新初始化成默认 (native = 中文), 用户在别处切
    // 的英文状态就没了 —— 表现正是"一聚焦输入框输入法就自动变中文"。
    // 控件级别不要各自去关联/断开。
    void updateImeContext();

    // 当前焦点是否接受输入法输入 (窗口有焦点 && 焦点控件 imeEnabled())。
    // false 时: 组字被取消、IME 的所有 UI (候选窗/组字窗/状态窗) 一律不显示,
    // 但 IMC 保持关联, 中英状态不动。
    bool imeActive() const { return m_imeActive; }

    // 让 IME 的组字窗/候选窗使用与本窗口一致的字体, 并保证输入法处于打开状态。
    // 由文本控件在 focusInEvent 里调用, fontPixelSize 为其正文字号 (逻辑像素)。
    // 注: "输入法打开"是硬需求 (关闭则完全无法组字), 但**中英模式**默认不动,
    // 见 setImeForceNativeMode。
    void prepareIme(int fontPixelSize);

    // 文本控件获得焦点时是否强制切到中文(native) 模式。默认 false:
    // 尊重用户当前的中/英状态, 聚焦只保证输入法处于打开状态。
    // 置 true 则每次聚焦都把 IME 拧到中文 (参考实现 test_ime 的
    // kForceChineseOnFocus); 适合"纯中文录入"场景, 但会覆盖用户手动切的状态。
    void setImeForceNativeMode(bool on) { m_imeForceNative = on; }
    bool imeForceNativeMode() const { return m_imeForceNative; }

    // 放弃当前未上屏的组字 (ImmNotifyIME CPS_CANCEL); imeActive() 转 false 时调用
    void cancelImeComposition();

    // 主动通知 IME 更新 UI 显示状态 (重发一次 WM_IME_SETCONTEXT)。
    // 必须显式发: 这条消息通常只在窗口获得/失去焦点时由系统发一次, 而那一刻焦点
    // 控件往往还不存在 (imeActive() 为 false), IME 就记住了"本窗口不显示任何 UI",
    // 之后开始组字也不再弹候选窗。组字窗仍由控件自绘, 永远不交给 IME。
    // IMC 全程保持关联, 所以重发它不会重置中/英状态。
    void updateImeUi(bool show);

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

    // 关闭窗口 (Qt 习惯, WM_CLOSE 与业务代码都调它):
    // 同步 sendEvent(JZCloseEvent) -> closeEvent() 让重写方 accept/ignore 表态,
    // 接受则隐藏 + 销毁原生窗口。返回是否真的关闭。
    // 两条路径都会同步 jzApp 的顶层窗口状态 (notifyWindowClosed), 由它判定
    // "最后一个可见顶层窗口已关"并退出事件循环 —— 否则进程残留。
    bool close();
    // 原生窗口已销毁 (WM_DESTROY): 清宿主状态避免句柄悬空, 之后 show() 可重建。
    void nativeDestroyed();

protected:
    // 显隐: show() 首次触发 showEvent -> 惰性创建原生窗口并显示;
    // hide() 触发 hideEvent -> 隐藏原生窗口 (未创建则为 no-op)。
    virtual void showEvent(JZShowEvent* event) override;
    virtual void hideEvent(JZHideEvent* event) override;
    // 关闭事件 (WM_CLOSE / close()): 重写方以 accept/ignore 表态。
    // 默认实现 = accept + close() —— 真正的关闭动作在这里执行。
    // (JZDialog 重写为 ignore + reject: 只隐藏不销毁, 可复用。)
    virtual void closeEvent(JZCloseEvent* event);
    virtual void paintEvent(JZPainterEvent* event) override;

    // UA 默认外观: 窗口白底 (换肤前的外观基线, 样式表未覆盖时回落到这里)
    JZStyleSet uaStyle() const override;
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
    bool m_scaleExplicit = false;        // 已显式设定 DPI (setDeviceScale), createWindow 不覆盖
    std::string m_title;
    WindowType m_windowType = Normal;
    bool m_top = false;                 // 置顶

    JZWidget* m_focusWidget = nullptr;  // 当前焦点控件 (接收键盘/IME)
    JZWidget* m_mouseGrabber = nullptr; // 鼠标抓取控件 (grabMouse 后所有鼠标事件直发它)
    JZWidget* m_inputGate = nullptr;    // 输入屏蔽层 (setInputGate; 可见期间丢真实输入)
    JZWidget* m_hoverWidget = nullptr;  // 当前悬停的最深控件 (updateHover 派发 enter/leave)
    void* m_imeContext = nullptr;       // 窗口级 IMC 句柄 (首次需要时取得并缓存)
    bool m_hasWindowFocus = false;      // 原生窗口是否有键盘焦点
    bool m_imeGotResult = false;        // 本次组字是否已由 ResultStr 上屏
    bool m_imeActive = false;           // 当前焦点是否接受 IME (见 imeActive())
    bool m_imeForceNative = false;      // 聚焦时是否强制切中文模式 (见 setImeForceNativeMode)

    // 显示定位: 显式 moveScreen 记录 (等价 Qt WA_Moved) 优先于自动居中
    bool m_autoPosition = true;         // 显示时相对 owner 居中 (Modal/Popup)
    bool m_hasScreenPos = false;        // 用户显式 moveScreen 过
    int  m_screenX = 0;                 // 记录的屏幕逻辑坐标 (窗口未创建时暂存)
    int  m_screenY = 0;
};

#endif // JZ_WINDOW_H_

