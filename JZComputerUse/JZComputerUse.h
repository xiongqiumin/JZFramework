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

// 归一化矩形(相对虚拟桌面截图, [0,1])
struct CuRect JZFRAMEWORK_API { double x = 0, y = 0, w = 0, h = 0; };

// 外部真实桌面的 Computer Use 底层语义。
// 只提供"基本语义"(切桌面/点击/按键/拖拽) + **感知** + "查找";
// 不构建控件树 —— 控件树与 UI 操作全部在 JS 侧(computeruse 包)完成。
// 感知的**三条腿**(见 docs/compute-use-design.md §2/§7):
//   ① 布局(正路) = 自家窗口走框架语义树 / **外部软件先走 UI Automation**、退路 Win32 子窗口枚举;
//   ② 像素族(兜底) = matchImage / matchColor —— 默认**不编**(要启用就定义 JZ_CU_OPENCV=1);
//   ③ OCR   (兜底) = matchText / ocrTexts —— 默认**不编**(要启用就定义 JZ_CU_OCR=1 + JZCv);
// ②③ 都是"显式退化"路径: 没编入 / 没识别到一律返回"空 / 未找到", 绝不假装成功。
class JZFRAMEWORK_API JZComputerUse {
public:
    enum class MouseBtn { Left, Right, Middle };

    // —— 基本语义 ——
    static void switchToDesktop();                                 // 显示桌面 (IShellDispatch4::MinimizeAll, 非按键)
    static bool click(double nx, double ny, MouseBtn btn = MouseBtn::Left);
    static bool doubleClick(double nx, double ny, MouseBtn btn = MouseBtn::Left);
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

    // —— 布局通道（运行期寻址的**正路**；像素只在建模期与退化兜底用，见 docs §4.1）——
    // 导出可访问布局树(JSON; rect 已归一化, 与 click/screenshot 同一口径):
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
    static bool waitStable(int settleMs, int timeoutMs);
    // 启动 / 激活目标窗口: exe 非空则先启动(args 可选), 再按 title 片段等窗口出现并置前。
    static bool launch(const std::string& exe, const std::string& args,
                       const std::string& title, int timeoutMs);

    // —— 截图 / 查找 ——
    static std::string screenshot();                               // 虚拟桌面截图 -> dataURL
    static CuRect    matchImage(const std::string& templatePath, const CuRect& region); // 模板匹配, 未找到 w==0
    // 颜色查找: 返回最**大**的一块同色区域(未找到 w==0)。
    // ⚠ 没编 OpenCV 的构建(如 virtual_device 容器)走**纯 C++ 兜底**: 扫帧 + 连通块取最大,
    //   容差与归一化口径与 OpenCV 版一致 ⇒ "容器里的脚本按颜色找东西"这条路是可用的。
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
    // 屏幕上所有文字: [{text,score,x,y,w,h}]（rect 归一化, 与 layoutTree/click 同口径, 便于直接点）
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
    //   layoutTree(title)   -> 桌面里那个窗口的语义树, rect 归一化到**整块桌面**(与 click 同口径)
    //   click/drag          -> 注入虚拟桌面 (命中窗口 -> 激活它 -> 换算客户区 -> 投递控件)
    //   key/type            -> 投递给虚拟桌面的活动窗口
    //   focusWindow(title)  -> 激活桌面上那个窗口
    //   waitStable()        -> 恒真 (虚拟桌面渲染是同步的, 不存在"界面还没渲染完")
    // 虚拟桌面下**不适用**的: switchToDesktop / 进程管理(start/terminate/focusProcess) /
    //   launch —— 一律明确返回失败, 绝不悄悄去动真实系统。
    //   (像素族匹配在虚拟桌面下**可用**: 帧就是桌面合成帧, 见 currentFrame; 未编 OpenCV 时
    //    走纯 C++ 兜底, 见 matchColor)
    static void setVirtualDesktop(JZVirtualDevice* desk);   // nullptr = 回到真实桌面
    static JZVirtualDevice* virtualDesktop();
    static bool isVirtualDesktop();

private:
    static CuRect s_lastLogical;                                   // 最近截图对应的虚拟桌面逻辑矩形(坐标换算用)
    static JZVirtualDevice* s_virtualDesk;                         // 虚拟桌面后端 (nullptr = 真桌面)
    static void   updateLogical(const JZRect& r);
    static DWORD  btnDown(MouseBtn b);
    static DWORD  btnUp(MouseBtn b);
    static void   toAbsolute(double nx, double ny, DWORD& ax, DWORD& ay);
};
