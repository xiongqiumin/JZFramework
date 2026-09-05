#ifndef JZ_SCREEN_H_
#define JZ_SCREEN_H_

#include <memory>
#include <string>

#include "JZPoint.h"
#include "JZRect.h"
#include "JZImage.h"

// ==================== 屏幕抽象 (等价 Qt::QScreen) ====================
//
// 职责: 屏幕**几何**与 **DPI** 的权威来源, 以及整屏截图。
//   - 几何 (geometry/availableGeometry) 为**逻辑像素**: 物理像素 ÷ devicePixelRatio(),
//     与 Qt::QScreen::geometry() 一致 (Qt6 起也是 device-independent pixels);
//   - DPI 三件套对应 QScreen 的 logicalDotsPerInch / physicalDotsPerInch /
//     devicePixelRatio, 数据来自 GetDpiForMonitor (MDT_EFFECTIVE_DPI / MDT_RAW_DPI);
//   - 截图为**物理像素**图像 (与 JZWidget::grab() 一致, 截图即设备像素)。
//
// 与 JZDisplayDevice::scale() 的关系:
//   - JZScreen  = 屏幕级**查询源** (每屏一个 DPI, 随系统设置变化);
//   - JZDisplayDevice = 窗口级**当前生效缩放** (缓存值, 窗口创建时取自所属屏,
//     WM_DPICHANGED 时刷新; popup 可用 JZWindow::setDeviceScale 显式覆盖)。
//   二者是"查得到"与"正在用"的关系, 不冲突。
//
// 生命周期: 轻量值对象 (内部共享一份快照)。构造即抓取当前显示器信息,
// 显示器热插拔 / DPI 变更后请重新取 (primaryScreen() / screenAt() 重新构造)。
//
// 仅 Windows 实现 (同 JZWidget 模块: JZWidget.cmake 在非 Win32 直接 return)。
class JZScreen
{
public:
    JZScreen();                     // 无效屏 (isValid()==false)
    explicit JZScreen(int index);   // 按系统枚举顺序; 越界得到无效屏

    bool isValid() const;
    int index() const;
    std::string name() const;       // 设备名 (如 \\.\DISPLAY1), 无效屏返回空
    bool isPrimary() const;

    // 几何 (逻辑像素): 含任务栏 / 去掉任务栏的工作区
    JZRect geometry() const;
    JZRect availableGeometry() const;

    // DPI: logical 含系统缩放 (effective), physical 为面板原始 DPI
    // (不可得时回落 logical), devicePixelRatio = logical / 96。
    float logicalDotsPerInch() const;
    float physicalDotsPerInch() const;
    float devicePixelRatio() const;

    // 整屏截图 (**物理像素**); 无效屏返回 isNull() 空图
    JZImage grabScreen() const;

    // ---- 静态: 屏幕枚举与查询 ----

    static int count();
    static JZScreen primaryScreen();                  // 主屏 (无显示器时无效)
    static JZScreen screenAt(const JZPoint& globalPos); // 按逻辑屏幕坐标定位所属屏

    // 抓任意原生窗口客户区 (**物理像素**): PrintWindow 优先 (窗口被遮挡/最小化
    // 也能抓到自绘内容), 失败退化 BitBlt。本框架窗口建议用 JZWidget::grab()
    // (它会按 DPI 与控件矩形裁剪)。
    static JZImage grabWindow(void* nativeHandle);

    // 取原生窗口所在显示器的逻辑 DPI (nullptr = 主屏); 失败回落 96。
    // 是框架内 DPI 查询的唯一入口 (JZWindow 建窗/DPICHANGED/IME 字体都走它)。
    static float logicalDpiFor(void* nativeHandle);

private:
    struct Info;
    std::shared_ptr<Info> m_info;
};

#endif // !JZ_SCREEN_H_
