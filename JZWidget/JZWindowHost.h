#ifndef JZ_WINDOW_HOST_H_
#define JZ_WINDOW_HOST_H_
#include "JZGlobal.h"

class JZWindow;

// ==================== 窗口宿主 (谁来托管这台窗口) ====================
// 默认宿主 = 当前平台: 每个 JZWindow 都建一个**原生窗口** (Win32 = CreateWindowExW)。
// 装上别的宿主后, **之后新建的每个 JZWindow 都先问它** —— 于是"这批窗口跑在哪儿"变成
// 一句开关, 而不是散落在各处的 if:
//
//     JZVirtualDevice desk(800, 600);    // 一块虚拟桌面 (见 binary/virtual_device)
//     jzSetWindowHost(&desk);            // 之后 new 出来的窗口 (含 **JS 里 new 的**) 全在它上面
//
//     JZJSEngine engine;
//     engine.setWindowHost(&desk);       // 同上, 只是把开关绑在引擎的寿命上
//     engine.init();
//
// 机制: `new JZWindow(...)` 构造的最后一步回调宿主 (onWindowCreated) —— 宿主在这一刻
// **换掉它的平台后端** (JZWindow::setWindowBackend) 并做登记, 于是这台窗口的
// 坐标/显隐/激活/取帧/句柄全部落到宿主上 (虚拟桌面就是靠这个让 JS 建出"虚拟窗口")。
// ⚠ 回调必须发生在 show() 之前 (那时才会建平台窗口); 构造期调用满足这一点。
// ⚠ 宿主是**线程局部**的: 窗口归"创建它的线程"管 —— 每窗口一个线程/一个 JS 引擎时
//   (JZVirtualProcess), 各线程各装各的宿主, 互不顶掉。
// 对照: Qt 的 QPA (QPlatformIntegration 给每个 QWindow 建 QPlatformWindow) —— 这里是同一层
// 东西的可替换点, 只是粒度只到"窗口"这一个点上。
class JZFRAMEWORK_API JZWindowHost
{
public:
    virtual ~JZWindowHost() {}

    // 窗口对象建好了 (后端已装, 还没 show): 宿主在此登记 / 摆位置 / 换平台后端 /
    // 把"内部窗口"从宿主 app 的顶层窗口表里摘掉 (setTopLevelWindow(false))。
    virtual void onWindowCreated(JZWindow* w) { (void)w; }
};

// 当前的窗口宿主; 没装 (nullptr) 时用当前平台的默认实现 (jzCreateDefaultWindowBackend)。
// ⚠ 不转移所有权: 宿主必须活得比它托管的窗口久 (调用方保证)。
JZFRAMEWORK_API void jzSetWindowHost(JZWindowHost* host);
JZFRAMEWORK_API JZWindowHost* jzWindowHost();

#endif // JZ_WINDOW_HOST_H_
