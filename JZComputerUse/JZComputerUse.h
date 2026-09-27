#pragma once
#include <string>
#include "JZWidget/JZScreen.h"             // JZRect / JZImage
#include "binary/code_agent/JZScreenShot.h"  // captureVirtualDesktop / toDataUrl / save

// 归一化矩形(相对虚拟桌面截图, [0,1])
struct CuRect JZFRAMEWORK_API { double x = 0, y = 0, w = 0, h = 0; };

// 外部真实桌面的 Computer Use 底层语义。
// 只提供"基本语义"(切桌面/点击/按键/拖拽) + "查找"(模板匹配/颜色匹配);
// 不构建控件树 —— 控件树与 UI 操作全部在 JS 侧(computeruse 包)完成。
// 注: 本文件未接入构建(CMake 未加), OpenCV / JZScreenShot 的链接在接入时补。
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

    // —— 截图 / 查找 ——
    static std::string screenshot();                               // 虚拟桌面截图 -> dataURL
    static CuRect    matchImage(const std::string& templatePath, const CuRect& region); // 模板匹配, 未找到 w==0
    static CuRect    matchColor(const std::string& hexColor, const CuRect& region);    // 颜色查找, 未找到 w==0

private:
    static CuRect s_lastLogical;                                   // 最近截图对应的虚拟桌面逻辑矩形(坐标换算用)
    static void   updateLogical(const JZRect& r);
    static DWORD  btnDown(MouseBtn b);
    static DWORD  btnUp(MouseBtn b);
    static void   toAbsolute(double nx, double ny, DWORD& ax, DWORD& ay);
};
