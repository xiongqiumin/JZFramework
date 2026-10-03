#pragma once
// ⚠ Win32 的类型(DWORD/HWND…)要在**本头文件内**可见, 且 NOMINMAX 必须早于第一处 <windows.h> ——
//   否则 min/max 宏会漏进后面所有框架头(JZStyleSet.h 那种 `std::min` 会被宏撕碎, 实测爆 C2589)。
//   本类只在 Windows 构建里出现(JZWidget.cmake 在非 Win32 直接 return), 故这里直接引。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include "JZWidget/JZScreen.h"             // JZRect / JZImage
#include "binary/code_agent/JZScreenShot.h"  // captureVirtualDesktop / toDataUrl / save
#include "json/json.h"                     // layoutTree 的返回(Json::Value 由 jsbind 自动转 JS 对象)

class JZVirtualDevice;                     // 虚拟桌面后端 (见 setVirtualDesktop)

// 一个矩形: **物理像素**, 相对整块屏幕 / 整块虚拟桌面 (x,y = 左上角, w,h = 尺寸)。
// ⚠ 2026-10-03 起**不再是归一化坐标**: 对外一律物理像素 (与截图、SendInput、UIA/MSAA 同口径),
//   见下面类注释里的"坐标口径"。
struct CuRect JZFRAMEWORK_API { double x = 0, y = 0, w = 0, h = 0; };

// 外部真实桌面的 Computer Use 底层语义。
// 只提供"基本语义"(切桌面/点击/按键/拖拽) + **感知** + "查找";
// 不构建控件树 —— 控件树与 UI 操作全部在 JS 侧(computeruse 包)完成。
//
// ⭐ 坐标口径 (2026-10-03 统一, 与 JZVirtualDeviceHttp 同一条规矩):
//   · **对外一律物理像素** —— 参数/返回值 (click/drag 的 x,y、CuRect、layoutTree 的 rect、
//     ocrTexts 的框) 都是"外面那个世界"的像素: 真桌面 = 屏幕物理像素 (抓屏 / SendInput /
//     UIA / GetWindowRect 都是它), 虚拟桌面 = 桌面逻辑 × 桌面 scale (合成帧就是它);
//   · **不再有归一化坐标** (旧契约是 0..1; 模型输出"图上的像素"由 JS 侧 perceive 内部
//     换算成物理 —— 那是它的私事, 不是对外口径);
//   · 框架内部 (JZWidget 的 rect/mapToGlobal、语义树 boundsInScreen、JZVirtualDevice 的
//     桌面坐标) 恒为**逻辑坐标** —— 换算只在本类边界上做一次 (见 toAbsolute / click 的
//     虚拟桌面分支 / layoutTree 的自家窗口分支)。
//   为什么: 外面那一圈原生全是物理像素; "窗口在中间、要按一个按钮"必须与它们同口径。
// 感知的**三条腿**(见 docs/compute-use-design.md §2/§7):
//   ① 布局(正路) = 自家窗口走框架语义树 / **外部软件先走 UI Automation**、退路 Win32 子窗口枚举;
//   ② 像素族(兜底) = matchImage / matchColor —— 默认**不编**(要启用就定义 JZ_CU_OPENCV=1);
//   ③ OCR   (兜底) = matchText / ocrTexts —— 默认**不编**(要启用就定义 JZ_CU_OCR=1 + JZCv);
// ②③ 都是"显式退化"路径: 没编入 / 没识别到一律返回"空 / 未找到", 绝不假装成功。
class JZFRAMEWORK_API JZComputerUse {
public:
    enum class MouseBtn { Left, Right, Middle };

    // —— 基本语义 ——
    // ⚠ 坐标都是**物理像素** (见类注释"坐标口径"): 真桌面 = 屏幕物理; 虚拟桌面 =
    //   桌面逻辑 × 桌面 scale (即 /api/screen 那张图的像素空间)。
    static void switchToDesktop();                                 // 显示桌面 (IShellDispatch4::MinimizeAll, 非按键)
    static bool click(double x, double y, MouseBtn btn = MouseBtn::Left);
    static bool doubleClick(double x, double y, MouseBtn btn = MouseBtn::Left);
    static bool drag(double x1, double y1, double x2, double y2);
    static void key(const std::string& name);                      // 单键或组合, 如 "Ctrl+C" / "Return"
    static void type(const std::string& text);                     // 任意文本(含中文), 按字符 SendInput
    static void wait(int ms);

    // —— 进程与窗口管理（不使用按键模拟）——
    static DWORD startProcess(const std::string& path, const std::string& args = ""); // 打开进程, 返回 PID, 失败返回 0
    static bool  terminateProcess(DWORD pid);                       // 按 PID 终止
    static bool  terminateProcess(const std::string& name);         // 按进程名终止(支持带/不带 .exe)
    static bool  focusProcess(DWORD pid);                           // 将某进程主窗口提到前台(切换进程)
    static bool  focusProcess(const std::string& name);             // 按进程名提前台
    static bool  focusWindow(const std::string& title);             // 按标题片段提前台(切换窗口)

    // —— 窗口表 / 窗口状态（"EnumWindows / SetWindowPos / ShowWindow / SetWindowText" 那一组）——
    // 两个后端**同一套语义**（这是"虚拟桌面里跑通的脚本要能在真机复验"的前提）:
    //   真桌面   = EnumWindows / SetWindowPos / ShowWindow / PostMessage(WM_CLOSE)
    //   虚拟桌面 = JZVirtualDevice::windowList / moveWindow / setClientSize / setMinimized ...
    // 坐标/尺寸一律**物理像素**（与 click/screenshot/screenInfo 同口径）。
    //
    // windows({title?}) -> 窗口表（z 序: 从下到上）
    //   [{handle, index, title, frame:[x,y,w,h], client:[x,y,w,h],
    //     visible, active, minimized, maximized}]
    //   handle = 不透明稳定标识（原样带回来给 windowState 用；下标会随建窗/关窗变）
    static Json::Value windows(const std::string& optsJson = std::string());
    // windowState({key, action?, x?, y?, w?, h?, title?}) -> 改完的那个窗口对象（同 windows 里的一项）
    //   key    = 句柄("0x…") | 标题片段 | 下标（字符串化; 虚拟桌面还认 "#下标"）
    //   action = "activate" | "minimize" | "maximize" | "restore" | "close"（缺省 = 只改几何/标题）
    //   x,y    = **外框**位置（物理）;  w,h = **客户区**尺寸（物理）;  title = 改标题
    //   找不到窗口 / 参数不对 -> null（不假装成功）
    static Json::Value windowState(const std::string& optsJson);

    // —— 布局通道（运行期寻址的**正路**；像素只在建模期与退化兜底用，见 docs §4.1）——
    // 导出可访问布局树(JSON; rect 是**物理像素**, 与 click/screenshot 同一口径):
    //   · 目标窗口属于**本进程** ⇒ 走框架语义树 `JZWidget::dumpSemanticTree`(带 viewId ⇒ 路径稳定)
    //   · **外部软件** ⇒ 先走**无障碍(UI Automation)**: 应用自己暴露的名字/角色/值/可操作模式 ——
    //     浏览器、Electron、WPF 这类自绘界面里有什么, 只有它说得清(Win32 枚举看到的只是空壳);
    //     拿不到(COM 失败 / 目标没实现 UIA / 只有空壳)才退回递归枚举 Win32 子窗口。
    //     ⚠ 虚拟桌面下 UIA 够不到(虚拟窗口没有原生 HWND) ⇒ 那条腿明确空, 不影响上面两条。
    // 目标窗口怎么定: `titleFilter` 非空 ⇒ 按标题片段找(这就是"**指定目标**"；脚本多半跑在
    //   终端里, 前台窗口是终端而不是被测程序, 靠前台猜必然猜错)；为空 ⇒ 用前台窗口。
    // 返回**信封** {source:'semantic'|'uia'|'win32', tree:{...}}；没有目标窗口、或两条腿都给不
    //   出东西 ⇒ null(调用方据此标 degraded 退到像素族)
    //   ⇒ JS 侧把节点标 degraded(显式退化, 不是静默兜底)。
    // ⚠ 布局路径(如 `/main/btnOutput`)由 **JS 侧**从这棵树上生成 —— C++ 只给"带 viewId 的树",
    //   路径规则只有一份实现(layout.ts), 两边各写一套必然漂移。
    static Json::Value layoutTree(const std::string& titleFilter = std::string());
    // 等界面稳定: 连续两帧画面相同且保持 settleMs ⇒ 稳定(超时返回 false)。
    // **像素在这里只当信号**(帧间变化收敛), 不参与寻址。
    // ⚠ 它在**两个后端是同一套** (真桌面抓屏 / 虚拟桌面读合成帧): 输入都是"送出去就返回",
    //   所以"动作生效了没"只能靠这一句观察 —— 动画/游戏这类一直在变的窗口会一直不收敛(超时)。
    static bool waitStable(int settleMs, int timeoutMs);
    // 启动 / 激活目标窗口: exe 非空则先启动(args 可选), 再按 title 片段等窗口出现并置前。
    static bool launch(const std::string& exe, const std::string& args,
                       const std::string& title, int timeoutMs);

    // 当前后端的尺寸与缩放 (给脚本查"屏多大 / 缩放多少"用; 不是一条必须走的换算步骤):
    //   { virtual: bool,                 是否虚拟桌面后端
    //     logical: [w,h],                框架逻辑尺寸 (虚拟桌面 = desktopW/H; 真桌面 = 逻辑虚拟屏)
    //     physical:[w,h],                物理像素尺寸 (= 截图尺寸; 真桌面 = 物理虚拟屏)
    //     scale:   double }              physical / logical (真桌面 = 屏的 devicePixelRatio)
    // ⚠ 对外坐标本来就是物理像素 ⇒ 从截图里量出的数直接就能点, 不需要先"换算"一次。
    static Json::Value screenInfo();

    // —— 截图 / 查找 ——
    // 截图: dataURL(PNG)。尺寸 = 该后端的**物理像素** (真桌面 = 屏幕物理; 虚拟桌面 =
    //   桌面逻辑 × scale) —— 与上面那些坐标同一个空间, 从图里读出的像素可以直接拿去点。
    static std::string screenshot();
    // 下面几个的 region 都是**物理像素** (可选的小搜索区; 缺省 = 整屏), 返回的 CuRect 也是
    // 物理像素 (x,y = 命中框左上角, w,h = 框尺寸); 未找到 ⇒ w == 0。
    static CuRect    matchImage(const std::string& templatePath, const CuRect& region); // 模板匹配(单尺度)
    // 颜色查找: 返回最**大**的一块同色区域(未找到 w==0)。
    // ⚠ 没编 OpenCV 的构建(如 virtual_device 容器)走**纯 C++ 兜底**: 扫帧 + 连通块取最大,
    //   容差与框口径与 OpenCV 版一致 ⇒ "容器里的脚本按颜色找东西"这条路是可用的。
    static CuRect    matchColor(const std::string& hexColor, const CuRect& region);

    // —— OCR（像素族兜底里的"文字"那一条；对应 JS 侧 Locator{kind:'ocr',text}）——
    // 本地 PP-OCR（JZCv/JZPaddleOCR, ONNX Runtime）—— 不依赖任何外部服务, 也不碰屏幕
    // (虚拟桌面下直接读桌面合成帧)。用途: 目标**没有可访问布局**时, 用文字定位。
    // ⚠ 这条是**显式退化**路径(docs §4.1): 没 init / 没模型 / 没识别到, 一律返回"空/未找到",
    //    绝不假装成功, 让上层能判出"这次的定位是退化来的"。
    // 模型目录: 显式传; 或环境变量 JZ_CU_OCR_MODEL; 或默认 <exe目录>/model/paddle_ocr。
    //   目录里两套布局都认: ① 开发布局 <root>/PP-OCRv6_small_{det,rec}/… (与 model/ 一致)
    //                       ② 平铺布局 <root>/{det.onnx,rec.onnx,dict.txt}
    static bool initOcr(const std::string& modelRoot = std::string());
    static bool ocrReady();
    static void deinitOcr();          // 释放模型占的内存（之后 ocrReady() 变 false）
    // 本构建是否**编入了** OCR —— 与 ocrReady() 不同: 那个是"模型加载好了没"。
    // 脚本先问 built() 才能区分两种情况: "这个 exe 没有 OCR 能力" vs "模型没配好"。
    static bool ocrBuilt();
    // 诊断信息: {built, ready, cls, modelRoot}。ready=false 时 modelRoot 往往就是问题所在
    //（没编入时额外带一句 hint）。
    static Json::Value ocrInfo();
    // 屏幕上所有文字: [{text,score,x,y,w,h}]（rect 是**物理像素**, 与 layoutTree/click 同口径,
    // 拿到就能直接点）。region 同样是物理像素。
    static Json::Value ocrTexts(const CuRect& region = CuRect(), double minScore = 0.6);
    // 按文字找: 命中返回归一化矩形; 未找到 w==0（等价 matchImage 的文本版）
    static CuRect matchText(const std::string& text, const CuRect& region = CuRect(),
                            double minScore = 0.6);

    // ————————————————————————————————————————————————
    // 后端选择: 真实桌面 / **虚拟桌面** (JZVirtualDevice)
    // ————————————————————————————————————————————————
    // 默认真实桌面(抓屏 + EnumWindows + SendInput)。传 JZVirtualDevice 之后, 上面**全部**
    // 屏幕/窗口/输入语义都改落在那块虚拟桌面上 —— 这是"确定性回放"那条路:
    //   screenshot()        -> 桌面合成帧 (不是抓屏; 不受遮挡/z 序/前台影响)
    //   layoutTree(title)   -> 桌面里那个窗口的语义树, rect 是**物理像素**(桌面逻辑 × 桌面
    //                          scale), 与 click/screenshot 同口径
    //   click/drag          -> 注入虚拟桌面 (命中窗口 -> 激活它 -> 换算客户区 -> 投递控件)
    //   key/type            -> 投递给虚拟桌面的活动窗口
    //   focusWindow(title)  -> 激活桌面上那个窗口 (最小化先还原; 返回"它真是活动窗口吗")
    //   waitStable()        -> **两个后端同一套**: 看画面收敛 (连续帧相同并保持 settleMs)
    // ⚠ 输入在两个后端是**同一个形状**: "送出去就返回, 由后端自己调度" ——
    //   真桌面 = SendInput 交给系统; 虚拟桌面 = 投递给那个窗口的线程 (见 JZVirtualWindow::injectMouse)。
    //   所以 click/drag/key/type 返回后动作可能还没被应用处理完 (真桌面那边也一样) ⇒ 要"动作后的
    //   画面/状态"就自己观察: waitStable 看画面收敛、或查窗口表/窗口标题。**脚本在两个后端行为一致**
    //   (这是"虚拟桌面里跑通的脚本要能在真机复验"的前提)。
    // 虚拟桌面下"进程/窗口管理"也走**同一套语义**(落在 JZVirtualDevice 上, 不碰真实系统):
    //   · switchToDesktop          -> 最小化桌面上所有窗口（同 IShellDispatch4::MinimizeAll 的语义）;
    //   · focusWindow(title)       -> 激活标题匹配的窗口（最小化先还原）;
    //   · focusProcess(name)       -> 激活那个进程的窗口（名字 = 程序名）;
    //   · terminateProcess(name)   -> 停掉那个进程（它建的窗口一起收走）;
    //   · windows()/windowState()  -> 窗口表 / SetWindowPos 那一组（move·resize·min·max·restore·close·title）。
    //   ⚠ 这些都**转投到桌面线程**执行（窗口表要抢桌面的锁, terminateProcess 还要 join 目标线程;
    //     在进程线程里直接做会踩线程亲和断言 —— 实测 FATAL 崩容器, 见 .cpp 里的 cuOnDesktopThread）。
    // 仍**明确失败**的两类:
    //   · 按**系统 pid** 的 terminateProcess(DWORD) / focusProcess(DWORD) —— 虚拟进程没有系统 pid, 按名字走;
    //   · startProcess / launch —— "起一个 JS 程序"的能力要 JS 引擎(jz_virtual_process 层), 而本文件
    //     会编进不同的目标(真桌面那条链里没有那个库) ⇒ 只能由宿主注入; 没注入就失败, 绝不假装成功。
    //   (像素族匹配在虚拟桌面下**可用**: 帧就是桌面合成帧, 见 currentFrame; 未编 OpenCV 时
    //    走纯 C++ 兜底, 见 matchColor)
    static void setVirtualDesktop(JZVirtualDevice* desk);   // nullptr = 回到真实桌面
    static JZVirtualDevice* virtualDesktop();
    static bool isVirtualDesktop();

private:
    static CuRect s_lastLogical;                                   // 最近截图对应的虚拟桌面逻辑矩形(坐标换算用)
    static JZVirtualDevice* s_virtualDesk;                         // 虚拟桌面后端 (nullptr = 真桌面)
    static DWORD  btnDown(MouseBtn b);
    static DWORD  btnUp(MouseBtn b);
    // 物理像素 -> SendInput 的绝对坐标 (0..65535, 覆盖**整个物理虚拟屏**)
    static void   toAbsolute(double x, double y, DWORD& ax, DWORD& ay);
};
