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
// 只提供"基本语义"(切桌面/点击/按键/拖拽) + "查找"(模板匹配/颜色匹配) + **布局导出**;
// 不构建控件树 —— 控件树与 UI 操作全部在 JS 侧(computeruse 包)完成。
// ⚠ 像素族匹配(matchImage/matchColor) 已降级为**兜底**(见 docs/compute-use-design.md §4.1),
//   默认**不编**(OpenCV 未接入构建): 要启用就定义 JZ_CU_OPENCV=1。
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
    //   · 否则 ⇒ 递归枚举 Win32 子窗口(标准控件够用; 自绘控件的细节要靠像素兜底 / UIA, 后续)
    // 目标窗口怎么定: `titleFilter` 非空 ⇒ 按标题片段找(这就是"**指定目标**"；脚本多半跑在
    //   终端里, 前台窗口是终端而不是被测程序, 靠前台猜必然猜错)；为空 ⇒ 用前台窗口。
    // 返回**信封** {source:'semantic'|'win32', tree:{...}}；没有目标窗口返回 null
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
    static CuRect    matchColor(const std::string& hexColor, const CuRect& region);    // 颜色查找, 未找到 w==0

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
    //   launch / 像素族匹配 —— 一律明确返回失败, 绝不悄悄去动真实系统。
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
