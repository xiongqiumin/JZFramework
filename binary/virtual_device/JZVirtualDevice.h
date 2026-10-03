#ifndef JZ_VIRTUAL_DEVICE_H_
#define JZ_VIRTUAL_DEVICE_H_
#include "JZWidget.h"      // JZMouseEvent / JZKeyEvent / JZColor / JZCursorShape
#include "JZWindowHost.h"  // 本类就是"窗口宿主"的一个实现 (见下)
#include "JZDisplayDevice.h"   // 合成画布 = 一块离屏显示设备 (它自己的帧缓冲就是屏幕)
#include "JZImage.h"
#include "JZPixmap.h"
#include "JZRect.h"
#include <atomic>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class JZVirtualWindow;
class JZVirtualScreenView;
class JZVirtualProcess;
class JZWindow;
class JZThread;
class JZPainter;

// ==================== 虚拟进程 ====================
// "一个窗口一个线程"的那半边: 每个进程 = 一个 JZThread + 一个 JZEngineRuntime(内含
// JZJSEngine) + 它建出来的窗口 (见 JZVirtualProcess.h)。
//
// 对桌面来说"进程"就只是**一个 JZThread** (停它/等它/查它在不在跑都是 JZThread 的现成
// 接口), 所以这里不需要什么抽象基类, 也不需要认识 JS 引擎 —— 桌面库与进程库因此是单向的:
//   jz_virtual_process  ->  jz_virtual_device
//
// 桌面(主线程) 与 进程线程 之间的通信只有三处 (都是框架现成机制, 不是新协议):
//   ① 建窗/关窗/改窗口状态: 进程线程直接调桌面的方法, 桌面内部**一把递归锁**罩住窗口表;
//   ② 注入输入 (点/键/滚轮)/关窗: 一律**投递**给客户区那个线程 (同 PostMessage; 本线程窗口
//      直接派发), **投出去就返回** —— 与真桌面 SendInput 同一个形状(送出去, 由后端/系统调度),
//      所以两个后端的脚本语义一致; 要确认动作生效就自己观察画面/窗口表 (CU 的 waitStable);
//   ③ 取帧 (合成时读客户区帧缓冲): 让**窗口自己的线程**拷一帧 (jzRunOnThreadSync, 见
//      JZVirtualThread.h), 桌面只拿一份拷贝, 不碰别人的帧缓冲;
//   ④ 进程**自己**结束 (未处理异常 / 线程体跑完): 进程线程投一个异步事件过来 -> 本类
//      terminateProcess 收尾 (摘表 + 收窗口 + delete, 见 JZVirtualProcess::finishProcess)。
//      所以桌面不需要"定期清扫死进程", 程序表里不会留跑完了的进程。

// ==================== 虚拟桌面 (虚拟显示器 + 窗口管理器) ====================
// 它就是"这台虚拟机的屏幕": 一块 desktopW × desktopH 的画布 + 其上的窗口管理器。
//   · 合成: 桌面底色 + 各窗口 (按 z 序由下到上) 的**外框 + 客户区帧**, 直接画进画布
//     (m_canvas, 一块离屏显示设备) 的原生缓冲 —— 显示控件贴它的零拷贝位图
//     (desktopPixmap), 需要 RGBA 时才按需转一份 (desktopImage);
//   · 窗口 = JZVirtualWindow (外框) + 它里面那个真 JZWindow (客户区, 渲染/输入走框架同一条链);
//   · 坐标: 桌面坐标 == 屏幕**逻辑**坐标 (左上角原点) ⇒ 与真桌面同构,
//     screenToClient / clientToScreen / "点击落在哪个窗口" / 归一化 rect 全部成立;
//     ⚠ 缩放 (scale, 见 setScale) 只决定"渲染成多少物理像素"; **框架内部的坐标恒为逻辑**
//        (命中/注入/几何都是), 但**对外接口 (HTTP / CU / 截图) 一律物理像素** ——
//       坐标/命中/注入/归一化 rect 在任何 DPI 下都一模一样;
//   · 输入: 给**桌面坐标**, 自动命中窗口 + 激活 + 按命中区域分发 ——
//     标题栏=拖动窗口 / 边框=缩放 / 关闭=关窗 / 最小化·最大化=改窗口状态 /
//     客户区=换算成客户区坐标投递给那个 JZWindow (与真实桌面点击同语义);
//   · 桌面 chrome: 底部**任务栏** (左边开始按钮 + 每个可见窗口一个按钮) 与它弹出来的
//     **开始菜单** —— 由桌面自己画 (不是窗口), 恒在窗口之上; 于是"最大化"只铺到工作区
//     (桌面减掉任务栏), 不会盖住任务栏/开始菜单; 任务栏上点窗口按钮 = 激活/还原它。
//     菜单项由宿主登记 (桌面不认识"新建窗口/起进程"这类动作, 见 addStartMenuItem)。
//   · 取帧: screenshot() 整屏 / screenshotWindow(w) 单窗口 —— 直接读帧缓冲, 不抓屏。
//
// 它**不是窗口**: 显示交给宿主 (JZVirtualScreenView + JZMainWindow, 见 main.cpp);
// 不显示也能完整工作 (无头 / ctest / 确定性回放)。
//
// ⭐ 它同时是**窗口宿主** (JZWindowHost): 装上之后, 这台进程里**之后新建的每个 JZWindow**
// (含 JS 里 `new JZWindow(...)`) 都会落到这块桌面上 —— 见 onWindowCreated:
//     jzSetWindowHost(&desk);           // 或 JZJSEngine::setWindowHost(&desk)
//     new JZWindow(400, 300, "t")       // C++ / JS 都一样: 建出来就是虚拟窗口
//        -> onWindowCreated(w)          // 建外框对象 + 把 w 的后端换成虚拟后端 + 登记
// 于是"让 JS 建的窗口都是虚拟的"= 一句开关, 不需要 JS 侧或绑定层配合。
//
// 与宿主 app 的关系: 桌面内部的窗口不登记进 JZApplication 的顶层窗口表 (宿主会给它们
// setTopLevelWindow(false)), 也不参与其"活动窗口" 判定, 所以"虚拟桌面在跑"不会干扰宿主
// app 的窗口生命周期判定。
class JZVirtualDevice : public JZObject, public JZWindowHost
{
public:
    JZVirtualDevice(int desktopW = 800, int desktopH = 600);
    ~JZVirtualDevice();

    // ---- JZWindowHost ----
    // 窗口诞生 (还在构造里, 未 show): 收进桌面 —— 建外框对象 (JZVirtualWindow)、把客户区的
    // 平台后端换成 JZVirtualWindowBackend (= "把 windows 后端设成虚拟后端")、登记 z 序,
    // 并把它标成"内部窗口"(不进宿主 app 的顶层窗口表)。
    void onWindowCreated(JZWindow* w) override;

    // 收下一个窗口 (owner=nullptr 表示"当前线程就是它的主人"; 进程线程建窗时由那里的
    // 窗口宿主传自己的线程)。**跨线程安全** (内部一把递归锁罩住窗口表)。
    JZVirtualWindow* adoptWindow(JZWindow* client, JZThread* owner);

    // ---- 进程管理 (每个进程 = 一个 JZThread + 一份 JS 运行时) ----
    // 对桌面来说进程就是"一个线程": 停它/等它/查它都是 JZThread 的接口, 桌面不认识引擎。
    // 用法:
    //     JZVirtualProcess* p = desk.createProcess("app1", script);   // 见 JZVirtualProcess.h
    const std::vector<JZThread*>& processes() const { return m_processes; }
    int processCount() const { return (int)m_processes.size(); }
    void terminateProcess(JZThread* p);               // 停 + 等 + 收它的窗口 + 删
    void shutdownProcesses();                         // 全部收掉 (析构时自动调)
    void addProcess(JZThread* p);                     // 由进程自己登记 (JZVirtualProcess::start)

    // ---- 程序输出 (console.log 等) 的环形缓冲 ----
    // 容器里每个进程的 console.* 都路过这里 (见 JZVirtualProcess 的 logHandler): 一份照旧打到
    // 容器 stdout, 另一份进这个**最近 kOutputLines 行**的缓冲 —— 外部可以直接
    //     GET /api/logs?offset=<行号>&size=<条数>       (size < 0 = 读到末尾)
    // 按行取, 不必去 grep 容器的 stdout 重定向(那是整个进程的输出, 还混着容器自己的日志)。
    // ⚠ 行号是**绝对**的 (累计行号, 已被挤掉的也算): outputTotal() = 一共产生过多少行,
    //   outputFirst() = 缓冲里还留着的第一行的行号 ⇒ 消费方记住 total 就能增量拉取。
    static const int kOutputLines = 1000;
    void appendOutput(const std::string& line);            // 线程安全 (由进程线程调用)
    int outputTotal() const;                               // 累计行数 (含已被挤掉的)
    int outputFirst() const;                               // 缓冲里第一行的绝对行号
    // 取行: offset = 起始**绝对**行号 (给得太早 -> 从最老的开始; 超出末尾 -> 空),
    //       size <= 0 = 一直到末尾。行文本形如 `[vproc: <进程名>] <原始输出>`。
    std::vector<std::string> outputLines(int offset, int size) const;   // 线程安全

    // 建并启动一个进程: 一个线程 + 一份 JZEngineRuntime(内含 JZJSEngine), 里面**加载程序**。
    // 程序里 `new JZWindow(...)` 建出来的窗口就属于该进程 (线程独立、引擎独立)。
    //   moduleRel: 非空 = 以一个 **JS 程序 (ESM 模块)** 加载 (相对 workDir) —— 推荐这条路:
    //              import 能用, 且 jzApp.applicationDirPath() = 程序所在目录;
    //   script   : 只给 script = 当一段 JS 跑 (片段/脚本体, 无模块语义)。
    //   ⚠ 实现见 JZVirtualProcess.cpp (jz_virtual_process 库) —— 桌面库不依赖 JS 引擎。
    //   ⚠ 新建的窗口由**那个进程的线程**创建, 所以本函数只负责"起线程", 窗口稍后出现
    //     (要等就 JZVirtualProcess::waitWindow)。
    JZVirtualProcess* createProcess(const std::string& name, const std::string& script,
                                    const std::string& workDir = std::string(),
                                    const std::string& moduleRel = std::string());

    // 新窗口的默认位置 (居中级联; 由 JZVirtualWindow 首次 show 时按最终尺寸取用)
    JZPoint defaultWindowPos(int clientW, int clientH) const;

    int desktopWidth() const { return m_desktopW; }
    int desktopHeight() const { return m_desktopH; }
    JZSize desktopSize() const { return JZSize(m_desktopW, m_desktopH); }

    // ---- 底部任务栏 + 开始菜单 (桌面 chrome: 不是窗口, 恒在窗口之上) ----
    // 任务栏是"屏幕的一条", 不属于任何窗口: 它左边是开始按钮, 右边每个**可见**窗口一个
    // 按钮 (点它 = 激活那个窗口; 最小化的会被还原)。因为它是 chrome, 所以:
    //   · 最大化只铺到 workArea() (桌面减掉任务栏) —— 盖不住任务栏/开始菜单;
    //   · 任务栏那一条不参与窗口命中 (windowAt 返回 nullptr), 点它也不会漏到下面的窗口。
    static const int kTaskbarHeight = 28;      // 逻辑像素 (随桌面缩放按物理分辨率渲染)
    int taskbarTop() const { return m_desktopH - kTaskbarHeight; }
    // 窗口可摆/可最大化的范围 (同真实系统的 workArea; 坐标系不变, 只是少了一条任务栏)
    JZRect workArea() const { return JZRect(0, 0, m_desktopW, taskbarTop()); }

    // 开始菜单项 (由**宿主**登记动作; 点中后回调在**锁外**执行 —— 回调里可以建窗/起进程)
    int addStartMenuItem(const std::string& text, std::function<void()> onActivate);
    bool isStartMenuOpen() const { return m_startMenuOpen; }
    void setStartMenuOpen(bool on);

    // chrome 几何 (逻辑像素): 绘制与命中共用这一份公式 —— 测试/自动化要"点得准"就用它,
    // 不要另抄一套常量 (抄了就会漂)
    JZRect startButtonRect() const;
    JZRect taskButtonRect(int index) const;      // index = 可见窗口在 z 序里的下标
    JZRect startMenuRect() const;
    JZRect startMenuItemRect(int item) const;

    // ---- DPI / 缩放: 这块桌面"有多少物理像素" ----
    // 两个空间, 与真机同构 (真机 = "逻辑分辨率 + 显示器缩放"):
    //   · **逻辑空间** = desktopWidth × desktopHeight (默认 800×600) —— 框架内部坐标:
    //     窗口几何 / 命中 / 事件 / 注入 / 布局, 恒为逻辑, **与 DPI 完全无关**;
    //   · **物理空间** = 逻辑 × scale —— 宿主界面看到的、截图返回的、HTTP/CU 收发的
    //     (对外一律物理, 见 JZVirtualDeviceHttp 与 JZComputerUse 的注释)。
    // scale 的来源有两个 (这就是"虚拟机 DPI 以后可以和主机不一样"那个口子):
    //   · **跟随宿主** (默认, 未显式设置): 由显示控件把宿主窗口的设备缩放同步进来
    //     (JZVirtualScreenView::syncHostScale -> setHostScale) ⇒ 与主机共享 DPI,
    //     贴图 1:1 不重采样, 文字最清晰;
    //   · **显式设置**: `--dpi/--scale` 或 `POST /api/desktop` (setScale) ⇒ 固定下来,
    //     宿主再变也不覆盖 (这块虚拟屏有自己的 DPI); 此时显示控件会按
    //     宿主scale/桌面scale 缩放贴图 (桌面 scale 小于宿主时会重采样, 是固有代价)。
    // 默认 1.0 = 纯逻辑桌面: 无头 / ctest 下物理 == 逻辑, 与宿主显示器无关, 回放可复现。
    void setScale(float scale);            // 显式设置 (此后不再跟随宿主)
    void setHostScale(float scale);        // 宿主同步 (仅未显式时生效)
    void followHostScale();                // 清除显式设置, 恢复跟随宿主
    bool isScaleExplicit() const { return m_scaleExplicit; }
    float scale() const { return m_scale; }
    // 逻辑像素 -> 物理像素 (帧缓冲/合成图尺寸都用它)
    int toPhysical(int logical) const
    {
        return (int)std::lround((double)logical * (double)m_scale);
    }
    // 物理像素 -> 逻辑像素 (对外接口换算用: HTTP/CU 收物理, 内部要逻辑)
    int toLogical(int physical) const
    {
        return (int)std::lround((double)physical / (double)(m_scale > 0.0f ? m_scale : 1.0f));
    }
    int physicalWidth() const { return toPhysical(m_desktopW); }
    int physicalHeight() const { return toPhysical(m_desktopH); }

    // 合成序号: 每合成一帧 +1。画布是"同一块缓冲、每帧重画"的可变目标 —— 需要 RGBA
    // 快照的消费方 (截图/CU) 据此判断自己缓存的那份是否过期 (见 desktopImage)。
    unsigned frameSerial() const { return m_frameSerial; }

    // 桌面底色 (默认**白屏**)。换成灰之类便于肉眼区分"桌面"与"窗口" —— 两者都是白的话
    // 窗口在整屏帧里看不出来。只影响底色, 不影响任何坐标/语义。
    void setDesktopBackground(const JZColor& c);
    const JZColor& desktopBackground() const { return m_desktopBg; }

    // ---- 显示 (可选): 宿主界面里显示这块屏幕的控件 ----
    // 只是"显示器", 不参与任何窗口/坐标/输入语义; 不设它也能跑 (纯无头)。
    void setView(JZVirtualScreenView* view);
    JZVirtualScreenView* view() const { return m_view; }
    // 合成 + 让显示控件重绘 (无控件时纯无头)。
    // 拖动/缩放窗口**进行中**时走**同步**重绘 (窗口跟着光标走), 其余情况交给消息循环
    // 合并重绘 —— 见 refreshNow。
    void refresh();

    // ---- 多窗口管理 ----
    // 建窗口 (默认居中并按已有窗口数级联偏移, 避免完全叠住); 新窗口置顶并激活。
    JZVirtualWindow* createWindow(int w, int h, const std::string& title = "Window");
    JZVirtualWindow* createWindow(int w, int h, const std::string& title, int x, int y);
    // 硬删一个窗口 (外框 + 客户区都不要了): 从桌面表里摘掉并销毁。**不是"关窗"** ——
    // "关窗"是 JZVirtualWindow::close() (= PostMessage WM_CLOSE), 点 X / DELETE /api/windows
    // 都走那条 (它只把窗口从桌面摘下来, 客户区对象保留)。
    // 这里只给"收摊"用: terminateProcess / shutdownProcesses / 本类析构。
    // ⚠ 客户区在别的线程时, 删除动作会被投到它自己的线程 (跨线程 delete 会踩 JZObject 的
    //   线程亲和断言) —— 所以那条路是**异步**的, 返回时窗口可能还在表里 (下一投消失)。
    void destroyWindow(JZVirtualWindow* w);

    // ---- "销毁平台窗口" / "重新显示" (关窗那条路用它俩) ----
    // 客户区在自己线程上 close() 成功后: 把窗口**从桌面摘下来** —— 不再合成/命中/出现在
    // windows() 里, 但对象与客户区都留着 (客户区归 JS; 它的平台后端还指着这台外框,
    // 所以外框不能删)。之后客户区 show() 会把同一台外框重新挂回来 (== 重建 HWND)。
    // 摘下来的外框记在 m_detached 里, 由 pruneWindows 在客户区真被回收时收掉。
    void detachWindow(JZVirtualWindow* w);
    void attachWindow(JZVirtualWindow* w);      // 重新挂回桌面 (幂等; 没摘过就什么都不做)
    // 这台窗口还在桌面上吗 (只比指针, **不解引用** —— 给"投递迟到"的调用方判活用)
    bool hasWindow(const JZVirtualWindow* w) const;

    // 窗口列表 (z 序: 由下到上, 末尾最上)。
    // ⚠ 这是那张**活表**: 进程线程建窗/客户区被回收都会动它。跨线程或"先查后用"的调用方
    //   请用 windowList() 拿一份快照 (同线程、遍历中不建/不关窗时直接用它没问题)。
    const std::vector<JZVirtualWindow*>& windows() const { return m_windows; }
    std::vector<JZVirtualWindow*> windowList() const;                   // 快照 (锁内拷一份)
    // 某个进程建的窗口 (z 序); 传 nullptr = 宿主自己建的那些 —— "查进程窗口"用这个
    std::vector<JZVirtualWindow*> windowsOfProcess(JZThread* p) const;
    JZVirtualWindow* activeWindow() const { return m_active; }
    void raise(JZVirtualWindow* w);              // 置顶 (只改 z 序)
    void activate(JZVirtualWindow* w);           // 置顶 + 键盘焦点 (切换活动窗口)
    void moveWindow(JZVirtualWindow* w, int x, int y);
    // 按标题片段找窗口 (等价 CU 的 titleFilter)
    JZVirtualWindow* byTitle(const std::string& title) const;
    // 桌面坐标命中: z 序自上而下第一个包含该点的**可见**窗口 (含外框); 无则 nullptr
    JZVirtualWindow* windowAt(int screenX, int screenY) const;

    // ---- 坐标换算 (桌面坐标 <-> 窗口客户区坐标) ----
    JZPoint screenToClient(JZVirtualWindow* w, int screenX, int screenY) const;
    JZPoint clientToScreen(JZVirtualWindow* w, int clientX, int clientY) const;

    // ---- 合成 / 取帧 ----
    // ⚠ 合成只在**主线程**跑 (refresh 从别的线程调用时会被转投到主线程); compose 里读
    //   各窗口的客户区帧都是"让窗口自己的线程拷一份" (见 JZVirtualProcessBase::runSyncOnThread)。
    // 画布就是桌面自己的那块**离屏显示设备** (m_canvas): 后端把外框 + 客户区帧直接画进
    // 它的帧缓冲 (原生布局, 与真窗口同一条渲染路径), 没有 RGBA<->PARGB 的全图往返。
    // 尺寸 = 桌面逻辑尺寸 * scale (物理像素; 见 setScale)。
    // 整屏一帧 (RGBA8888, 物理像素): **按需**从画布转一份并缓存 (截图 / CU / 测试用;
    // 拖动路径不碰它 —— 显示控件贴的是下面的零拷贝位图)。
    const JZImage& desktopImage();
    // 画布的零拷贝显示位图 (内容只读, 每帧由 compose 重画在同一个缓冲上)。
    // 显示控件直接贴它 ⇒ 一次拷贝/转换都没有。
    const JZPixmap& desktopPixmap() const { return m_desktopPix; }
    // 桌面底色 + 各窗口 (按 z 序) 画进画布。合成中不重绘窗口 (用它们现有帧缓冲)。
    void compose();
    JZImage screenshot();                               // 整屏一帧 (物理像素)
    JZImage screenshotWindow(JZVirtualWindow* w);       // 客户区一帧 (物理像素)

    // ---- 输入注入 (坐标 = 桌面屏幕逻辑坐标) ----
    // 命中窗口 -> 激活它 -> 按命中区域分发 (拖动/缩放/按钮/客户区投递)。
    bool injectMouseAtScreen(JZMouseEvent::MouseType type, JZMouseEvent::Button button,
                             int screenX, int screenY, int wheelDelta = 0,
                             uint32_t modifiers = JZKeyEvent::NoModifier);
    bool injectClickAt(int screenX, int screenY,
                       JZMouseEvent::Button button = JZMouseEvent::LeftButton);
    // 键盘发给当前活动窗口
    bool injectKey(uint32_t key, const std::string& text = std::string(),
                   uint32_t modifiers = JZKeyEvent::NoModifier, bool press = true);
    // 按 UTF-8 逐字符输入 (可打印字符; 组合键/IME 走 injectKey / ImeEvent)
    bool injectText(const std::string& utf8);
    // 输入法事件 (组字/上屏/候选定位): 发给当前活动窗口 (见 JZVirtualWindow::injectIme)。
    // 宿主界面里它就是"把宿主窗口收到的 WM_IME_* 转投到虚拟桌面上"。
    bool injectIme(const JZImeEvent& ev);
    // ---- 输入法 (IME) 与宿主窗口之间的一条桥 ----
    // 桌面上"现在该不该有输入法" (由虚拟侧焦点控件的 imeEnabled 决定, 见 setImeWanted)。
    // JZVirtualScreenView::imeEnabled() 用它回答宿主窗口: 只有这时宿主才会给 IME 打开 UI,
    // 组字消息也才有去处 —— 否则用户在虚拟桌面里敲字时输入法是被"关"着的。
    bool imeWanted() const;
    // 上面那个结论变了时由虚拟窗口调用: 让**宿主那扇真窗口**重算一次 IMC
    // (跨线程时转投到主线程 —— 宿主窗口只能由它自己的线程碰)。
    void notifyImeChanged();
    // 在**显示控件自己的线程**上跑一段动作 (宿主相关的一切都归它; 无头时什么也不做)。
    // 虚拟侧(可能跑在任意虚拟窗口线程上)要推动宿主状态时走这一条, 别直接跨线程摸。
    void runOnView(std::function<void(JZVirtualScreenView*)> fn);
    // notifyImeChanged 的真身: 让宿主窗口按最新的 imeEnabled 重算 IMC
    static void updateHostImeContextOf(JZVirtualScreenView* v);

    // 桌面上该点应有的光标 (边框 = 缩放箭头, 其余 = 箭头) —— 宿主视图据此设光标
    JZCursorShape cursorAt(int screenX, int screenY) const;

    // ---- 内部: 由 JZVirtualWindow / 它的后端在帧/位置/状态变化时调用 ----
    void onWindowFrameChanged(JZVirtualWindow* w);

    // 正在拖动/缩放窗口 (窗口管理器内部状态)
    bool isDragging() const { return m_drag != DragNone; }

private:
    enum DragMode { DragNone, DragMove, DragResize };
    // 缩放边 (可组合: 左|上 = 左上角)
    enum { EdgeLeft = 1, EdgeRight = 2, EdgeTop = 4, EdgeBottom = 8, EdgeNone = 0 };

    void beginDragMove(JZVirtualWindow* w, int x, int y);
    void beginDragResize(JZVirtualWindow* w, int edges, int x, int y);
    void doDrag(int x, int y);
    void endDrag();
    // createWindow 两个重载的共同实现 (hasPos=false 时等首次 show 自动摆位)
    JZVirtualWindow* createWindowImpl(int w, int h, const std::string& title,
                                      bool hasPos, int x, int y);
    // ---- 任务栏 / 开始菜单的命中与绘制 ----
    int taskButtonAt(int x, int y) const;        // 命中任务栏按钮 -> 下标, 否则 -1
    int startMenuItemAt(int x, int y) const;     // 命中开始菜单项 -> 下标, 否则 -1
    void paintTaskbar(JZPainter& p);
    void paintStartMenu(JZPainter& p);
    // 点在 chrome 上 (锁内调用): 返回 true = 这次点击归桌面, 不再往下投给窗口;
    // pending = 需要在**锁外**执行的动作 (菜单项回调, 里面可能建窗/起进程)
    bool handleChromePress(int x, int y, std::function<void()>& pending);
    bool updateChromeHover(int x, int y);        // 悬停反馈; true = 光标在 chrome 上
    void activateTaskButton(int index);

    // 收掉"客户区已经没了"的窗口 (JS 侧把窗口对象回收掉时会发生; 守卫能判定)
    void pruneWindows();
    // 画布尺寸/缩放变了就同步后端帧缓冲, 并刷新它的零拷贝显示位图 (见 m_canvas)
    void ensureCanvas();
    void refreshNow();                  // 合成真身 (只允许主线程)
    void setScaleValue(float scale);    // setScale / setHostScale 的共同真身 (不做"谁设的"判断)
    // 把 w 从窗口表 / "已关闭"表里摘下来 (锁内; 调用方负责锁) —— 返回"活动窗口没了的话,
    // 焦点该交给谁"(锁外再调它的 setFocus); 不发生就返回 nullptr。
    JZVirtualWindow* removeWindowLocked(JZVirtualWindow* w);

    // 窗口表 + 活动窗口 + 合成: 一把递归锁罩住 (进程线程建窗/取帧 vs 主线程合成)。
    // 用递归锁是因为这些方法互相调用 (adopt -> setFramePos -> refresh -> compose)。
    mutable std::recursive_mutex m_mutex;

    std::vector<JZThread*> m_processes;               // 进程表 (就是它们各自的线程对象)

    // ---- 程序输出的环形缓冲 (见 appendOutput / outputLines) ----
    // 用**独立的一把锁**而不是 m_mutex: 进程线程写日志时绝不能去抢桌面那把锁 ——
    // 主线程可能正拿着它等这个进程(投递输入是同步 RPC), 抢了就是死锁。
    mutable std::mutex m_outputMutex;
    std::deque<std::string> m_outputs;     // 最近 kOutputLines 行 (超了从头弹)
    long long m_outputTotal = 0;           // 累计产生过的行数 (含已被挤掉的)
    // "已有回主线程刷新在途"(去抖)。⚠ 原子而非锁: 进程线程在**投递输入的那一刻**可能被
    // 主线程等 (sendEvent), 这里再去抢 m_mutex 就会死锁 —— 这条路径必须无锁。
    std::atomic<bool> m_refreshPosted{false};

    std::vector<JZVirtualWindow*> m_windows;   // 桌面上的窗口, z 序: 末尾最上
    // 已"关闭"(从桌面摘下来)但客户区还活着的外框 —— 客户区的平台后端还指着它, 所以不能删;
    // 等客户区被 JS 回收 (守卫变空) 时由 pruneWindows 一起收掉。重新 show 会把它搬回 m_windows。
    std::vector<JZVirtualWindow*> m_detached;
    JZVirtualWindow* m_active = nullptr;       // 当前活动窗口 (持键盘焦点)
    JZVirtualScreenView* m_view = nullptr;     // 宿主界面里的"屏幕"控件 (可选)
    // ⚠ 成员顺序即析构顺序: m_desktopPix 引用 m_canvas 的帧缓冲, 必须比它先析构。
    JZDisplayDevice m_canvas;                  // 合成画布 (离屏设备; 它的帧缓冲就是"屏幕")
    bool m_canvasReady = false;                // 画布已 initOffscreen (首次之后只 resize)
    JZPixmap m_desktopPix;                     // 画布的零拷贝显示位图 (随画布重建刷新)
    JZImage m_desktopImage;                    // 整屏 RGBA 快照 (按需转, 见 desktopImage)
    unsigned m_imageSerial = ~0u;              // 上面那份快照对应的合成序号
    JZColor m_desktopBg;                       // 桌面底色 (默认经典绿, 见 ctor)

    // ---- 任务栏 / 开始菜单现场 ----
    struct StartMenuItem {
        std::string text;
        std::function<void()> onActivate;
    };
    std::vector<StartMenuItem> m_startItems;
    bool m_startMenuOpen = false;
    int m_hoverStartItem = -1;                 // 悬停的开始菜单项 (-1 = 无)
    int m_hoverTask = -2;                      // -2 = 无, -1 = 开始按钮, >=0 = 窗口按钮下标

    float m_scale = 1.0f;                      // 渲染缩放 (逻辑 -> 物理; 见 setScale)
    bool m_scaleExplicit = false;              // scale 是显式设的 (true) 还是跟随宿主 (false)
    unsigned m_frameSerial = 0;                // 合成序号 (见 frameSerial)
    int m_desktopW = 0;
    int m_desktopH = 0;
    bool m_composing = false;                  // 合成中: 忽略窗口的刷新通知 (防重入)

    // createWindow() 显式给了坐标: 这次收窗口时直接用 (否则等首次 show 自动摆位)
    JZPoint m_adoptPos;
    bool m_hasAdoptPos = false;

    // ---- 窗口管理器拖动/缩放现场 ----
    DragMode m_drag = DragNone;
    JZVirtualWindow* m_dragWin = nullptr;
    int m_dragDX = 0, m_dragDY = 0;            // 拖动: 光标相对外框原点的偏移
    int m_dragEdges = 0;                       // 缩放: 动的边
    JZRect m_dragFrame;                        // 缩放起始外框
    JZPoint m_dragOrigin;                      // 按下点 (桌面坐标)
    JZVirtualWindow* m_pressed = nullptr;      // 按下落在客户区的窗口 (release 还给谁)
};

#endif // JZ_VIRTUAL_DEVICE_H_
