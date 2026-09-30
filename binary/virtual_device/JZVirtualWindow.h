#ifndef JZ_VIRTUAL_WINDOW_H_
#define JZ_VIRTUAL_WINDOW_H_
#include "JZWidget.h"      // JZMouseEvent / JZKeyEvent / JZCursorShape / JZObjectPointer
#include "JZWindow.h"      // 客户区就是它 (client() 直接返回 JZWindow*)
#include "JZImage.h"
#include "JZRect.h"
#include <string>

class JZVirtualDevice;
class JZThread;
class JZPainter;

// ==================== 虚拟桌面上的一个窗口 (虚拟侧的"平台窗口") ====================
// 它把"窗口"拆成两半, 与真实系统一模一样:
//   · **客户区** = 一个真 JZWindow —— 控件树/布局/渲染/命中/焦点/事件分发全走框架那一条链,
//     与真窗口没有任何分叉 (这个 JZWindow 是别人 new 出来的: JS 里 `new JZWindow(...)` 也一样);
//   · **外框**   = 标题栏 + 四周边框 + 最小化/最大化/关闭按钮, 由本类绘制并做命中测试 ——
//     这就是"窗口管理器"的那半边 (真系统里它在 OS 的窗口管理器, 这里在虚拟桌面上)。
// ⚠ 本类**不是** JZWindow 的子类。虚拟窗口不是"另一种 JZWindow", 而是虚拟桌面管理的
//   一个窗口对象; 里面那个 JZWindow 才是客户区。于是:
//     JZWindow::nativeHandle() 在虚拟桌面上返回的正是**本对象** (虚拟侧的句柄),
//     两个平台都有句柄, 不再有"虚拟恒为 nullptr"的特例。
// ⚠ 坐标口径只有一处: clientToScreen/screenToClient —— JZVirtualWindowBackend 的坐标
//   换算直接落在这里 (客户区原点 = 外框原点 + 装饰尺寸)。
class JZVirtualWindow
{
public:
    // 装饰尺寸 (逻辑像素 —— 桌面坐标恒为逻辑坐标, 与桌面缩放无关, 见 JZVirtualDevice::scale)
    enum {
        kBorder = 2,      // 四周边框宽
        kTitleBar = 22,   // 标题栏高 (含上边框)
        kButton = 12,     // 标题栏按钮边长
        kButtonGap = 4,   // 按钮间距
        kMinClientW = 60, // 拖动缩放时客户区最小值
        kMinClientH = 40,
    };

    // 外框命中区域 (窗口管理器据此决定: 拖动 / 缩放 / 点按钮 / 投递客户区)
    enum HitRegion {
        HitNone = 0,
        HitClient,
        HitTitle,
        HitMinimize,
        HitMaximize,
        HitClose,
        HitBorderLeft,
        HitBorderRight,
        HitBorderTop,
        HitBorderBottom,
        HitBorderTopLeft,
        HitBorderTopRight,
        HitBorderBottomLeft,
        HitBorderBottomRight,
    };

    // 接管一个**已经建好的**客户区窗口: 外框归本类, 客户区归那个 JZWindow。
    // 只由 JZVirtualDevice 在"窗口诞生"时调用 (见 JZVirtualDevice::onWindowCreated) ——
    // 构造实参里的 JZWindow 就是 JS/C++ 刚 new 出来的那个, 于是"JS 建的窗口"直接变成虚拟窗口。
    // ⚠ 客户区的所有权交给本类 (析构时收掉); 若它已被 JS 侧先一步回收, 这里不会重复 delete
    //   (守卫 m_clientGuard 变空即可判定, 见 client())。
    JZVirtualWindow(JZVirtualDevice* screen, JZWindow* client);
    ~JZVirtualWindow();

    JZVirtualDevice* screen() const { return m_screen; }

    // ---- 归属: 这台窗口跑在哪个进程/线程 ----
    // owner 为空 = "当前线程自己建的同线程窗口" (单线程用法, 与以前完全一样);
    // 非空 = 一个虚拟进程 (JZVirtualProcess, 它就是一个 JZThread + 一份 JS 运行时) 建的
    // 窗口 —— 于是:
    //   · 客户区的事件派发/绘制都发生在那个线程上 (jzApp->sendEvent 跨线程同步投递);
    //   · 取帧要"让窗口自己的线程拷一份" (见 clientFrame)。
    void setProcess(JZThread* p) { m_process = p; }
    JZThread* process() const { return m_process; }
    JZThread* ownerThread() const { return m_process; }   // 空 = 当前线程(同线程窗口)

    // 客户区 (真 JZWindow): 控件挂到它下面; 宿主窗口被销毁(如 JS 侧 GC)后返回 nullptr。
    // ⚠ client() 可能为空 —— 桌面合成/命中前都会先跳过这种窗口 (见 JZVirtualDevice::pruneWindows)。
    JZWindow* client() const;
    bool isClientAlive() const { return client() != nullptr; }

    // 虚拟侧的平台句柄 (== client()->nativeHandle(), 也就是本对象)。
    void* nativeHandle() const;

    // 标题: **客户区窗口的标题** (单一来源 —— 它也是画标题栏时读的那个)
    std::string title() const;
    void setTitle(const std::string& title);

    // ---- 外框几何 (桌面坐标) ----
    int frameX() const { return m_x; }
    int frameY() const { return m_y; }
    int frameWidth() const;
    int frameHeight() const;
    JZRect frameGeometry() const;
    void setFramePos(int x, int y);
    // 外框矩形 -> 客户区尺寸 (小于最小尺寸时按下限截断)。最大化还原也走它。
    void setFrameGeometry(const JZRect& rc);

    // 客户区在桌面上的矩形; 客户区左上角 (输入换算/装饰绘制的锚点)
    JZRect clientGeometry() const;
    int clientX() const { return m_x + kBorder; }
    int clientY() const { return m_y + kTitleBar; }

    // ---- 坐标换算: **本窗口一套口径** (客户区 <-> 桌面) ----
    JZPoint clientToScreen(const JZPoint& pt) const;
    JZPoint screenToClient(const JZPoint& pt) const;

    // ---- 显隐 / 活动态 ----
    // show(): 就是客户区 JZWindow::show() —— "摆位 + 出首帧 + 置顶/聚焦"由平台后端在
    // 首次显示时统一做 (见 JZVirtualWindowBackend::show), 于是 JS 里 `win.show()` 与
    // 桌面自己的 createWindow() 走的是同一条路, 不会出现"少做了一步"。
    void show();
    void hide();
    bool isVisible() const;
    bool isActive() const;
    void activate();                 // 置顶 + 键盘焦点 (桌面管理器实现)
    void setFocus(bool got);         // 键盘焦点进/出 (== WM_SETFOCUS/WM_KILLFOCUS)
    // 关窗: 走框架的 close() (派发 closeEvent, 接受才隐藏) —— 与真窗口同语义
    bool close();

    // ---- 窗口状态 (标题栏按钮 / 桌面管理器) ----
    void setMinimized(bool on);
    bool isMinimized() const { return m_minimized; }
    void setMaximized(bool on);
    bool isMaximized() const { return m_maximized; }

    // ---- 自动摆位 ----
    // 窗口构造时尺寸往往还没定 (JS 是 `new JZWindow(300,200)` 之后才 resize/setGeometry,
    // 甚至完全不写尺寸) ⇒ "摆在哪儿"只能等 show() 才知道。开着它 = 首次 show 时按最终尺寸
    // 居中级联; 显式 setFramePos 会关掉它 (外部给了坐标就听外部)。
    void setAutoPosition(bool on = true) { m_autoPosition = on; }
    bool isAutoPosition() const { return m_autoPosition; }

    // ---- 渲染 / 取帧 (尺寸 = 客户区逻辑尺寸 * 桌面 scale, 即物理像素) ----
    void renderNow();                          // 客户区重绘一帧 (并通知桌面)
    JZImage snapshot(bool renderFirst = true); // 客户区一帧 (RGBA8888; 跨线程会等窗口线程)
    JZImage clientFrame();                     // 取帧: 跨线程时让窗口自己的线程拷一份 (会等它)
    // 客户区最近一帧的**拷贝**(由窗口线程在内容变化时刷新, 见 captureFrame) ——
    // 桌面合成只读它, 不跨线程碰别人的帧缓冲 ⇒ 合成永不阻塞在别的线程上。
    const JZImage& frame() const { return m_frame; }
    // 在**窗口自己的线程**上把当前帧缓冲拷进 m_frame; 不在本线程时只记脏(等它自己刷)。
    bool captureFrame();

    // ---- 输入注入 (坐标 = **本窗口客户区**逻辑坐标; 桌面路由时已换算好) ----
    bool injectMouse(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                     int x, int y, int wheelDelta = 0,
                     uint32_t modifiers = JZKeyEvent::NoModifier);
    bool injectClick(int x, int y, JZMouseEvent::Button button = JZMouseEvent::LeftButton);
    bool injectKey(uint32_t key, const std::string& text = std::string(),
                   uint32_t modifiers = JZKeyEvent::NoModifier, bool press = true);

    // ---- 窗口管理器 (JZVirtualDevice 合成/命中的那半边) ----
    // 画外框 (标题栏底色+标题+按钮+边框), 坐标 = 桌面坐标; active 决定配色。
    void paintDecoration(JZPainter& p, bool active) const;
    // 桌面坐标命中: 客户区 / 标题栏 / 三个按钮 / 八向边框 (不在外框内 = HitNone)
    HitRegion hitTest(int screenX, int screenY) const;
    // 该点对应的光标形状 (边框 = 缩放箭头; 其余 = 箭头)
    JZCursorShape cursorShapeAt(int screenX, int screenY) const;
    // 外框/内容变了 -> 让桌面重新合成 (窗口自己的动作与平台后端都走它)
    void notifyScreen();
    // 桌面的 DPI 缩放变了 (JZVirtualDevice::setScale): 客户区的设备按新缩放重建帧缓冲
    // (物理尺寸 = 客户区逻辑尺寸 * scale) 并重画一帧 —— 由桌面逐个窗口调, 桌面那边已把
    // 窗口表拷出来锁外调用 (本函数可能要跨线程等窗口自己的线程画完)。
    void applyDeviceScale();
    // 首次显示时按最终尺寸居中级联 (只做一次; 由平台后端的 show 调, 见 setAutoPosition)
    void applyAutoPosition();

private:
    JZImage clientFrameLocal();                // 本线程直接读帧缓冲 (不加任何跨线程等待)
    void renderNowLocal();                     // 本线程直接重绘 (不跨线程)

    // 标题栏里第 i 个按钮 (0=最小化 1=最大化 2=关闭) 的矩形 (桌面坐标)
    JZRect buttonRect(int index) const;

    JZVirtualDevice* m_screen = nullptr;
    JZThread* m_process = nullptr;     // 空 = 同线程窗口 (见 setProcess)
    JZWindow* m_client = nullptr;      // 客户区 (真 JZWindow; 本类持有其所有权)
    JZObjectPointer m_clientGuard;     // 客户区存活守卫: 它被别处销毁后这里自动变空
    JZImage m_frame;                   // 客户区最近一帧的拷贝 (见 captureFrame)
    bool m_frameDirty = false;         // 帧缓冲变过但还没拷 (别的线程改过状态)
    bool m_autoPosition = false;       // 见 setAutoPosition

    int m_x = 0;                       // 外框左上角 (桌面坐标)
    int m_y = 0;
    bool m_minimized = false;
    bool m_maximized = false;
    int m_restoreX = 0, m_restoreY = 0, m_restoreW = 0, m_restoreH = 0;  // 最大化前的客户区
};

#endif // JZ_VIRTUAL_WINDOW_H_
