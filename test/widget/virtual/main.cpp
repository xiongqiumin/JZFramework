// ==================== 虚拟设备 (JZVirtualDevice) 冒烟测试 ====================
// 验证"无原生窗口也能完整跑一棵控件树"这条链:
//   1) show() 不建 HWND, 但设备/帧缓冲就绪 (nativeHandle() == nullptr);
//   2) 渲染真的画进了帧缓冲 (帧里有非白像素 = 控件被绘制);
//   3) 注入点击走的是与真实输入同一条分发链 (命中测试 -> 控件事件 -> 状态变化);
//   4) 取帧与"当前状态"一致 (状态变了, 帧跟着变);
//   5) 帧可存盘供肉眼确认 (virtual_device_frame.png)。
//
// 这个测试**不创建任何真实窗口**, 因此可以进 ctest。
#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZVirtualDevice.h"
#include "JZCheckbox.h"
#include "JZImage.h"

static int g_fail = 0;

static void check(bool ok, const char* msg)
{
    if (ok) {
        printf("  [ ok ] %s\n", msg);
    } else {
        printf("  [FAIL] %s\n", msg);
        ++g_fail;
    }
}

// 帧里是否有非白像素 (窗口底色是白色 => 非白即"有控件被画出来")
static bool hasInk(const JZImage& img)
{
    if (img.isNull() || !img.data())
        return false;
    const uint8_t* px = img.data()->pixels();
    if (!px)
        return false;
    const int n = img.width() * img.height();
    for (int i = 0; i < n; ++i) {
        if (!(px[i * 4] == 255 && px[i * 4 + 1] == 255 && px[i * 4 + 2] == 255))
            return true;
    }
    return false;
}

// 两帧是否逐像素相同
static bool sameFrame(const JZImage& a, const JZImage& b)
{
    if (a.isNull() || b.isNull() || !a.data() || !b.data())
        return false;
    if (a.width() != b.width() || a.height() != b.height())
        return false;
    return memcmp(a.data()->pixels(), b.data()->pixels(),
                  (size_t)a.width() * a.height() * 4) == 0;
}

int main(int argc, char* argv[])
{
    JZApplication app;

    const int W = 320, H = 200;
    JZVirtualDevice dev(W, H, "VirtualDevice smoke");
    JZCheckbox box(&dev);
    box.setText("power");
    box.setGeometry(20, 20, 200, 40);
    box.show();

    dev.show();                       // 只建离屏设备, 不建原生窗口
    dev.injectFocus(true);            // 等价 WM_SETFOCUS: 让窗口"处于活动态"

    printf("[虚拟设备]\n");
    check(dev.nativeHandle() == nullptr, "show() 后仍无原生窗口 (nativeHandle == nullptr)");
    check(dev.renderNow(), "renderNow() 成功 (走的是与真实窗口同一重绘入口)");

    // ---- 取帧 ----
    JZImage f0 = dev.snapshot();
    check(!f0.isNull(), "snapshot() 取到帧");
    check(f0.width() == W && f0.height() == H, "帧尺寸 = 逻辑尺寸 320x200 (虚拟态 scale=1)");
    check(hasInk(f0), "帧里有绘制内容 (非全白 => 控件真的被画出来了)");

    // ---- 注入点击 -> 状态变化 ----
    check(!box.isChecked(), "初始未勾选");
    dev.injectClick(30, 40);          // checkbox 中心 (几何 20,20,200x40)
    check(box.isChecked(), "注入点击后已勾选 (命中测试与事件分发都生效)");
    dev.injectClick(30, 40);
    check(!box.isChecked(), "再点一次切回未勾选 (可重复)");

    // ---- 状态变化 => 帧跟着变 ----
    box.setChecked(false);
    JZImage off = dev.snapshot();
    box.setChecked(true);
    JZImage on = dev.snapshot();
    check(!sameFrame(off, on), "勾选状态不同的两帧像素不同 (渲染与状态一致)");

    // ---- 事件过滤器/输入蒙版之外的常规路径: 未命中任何控件不崩 ----
    dev.injectClick(300, 190);
    JZImage f2 = dev.snapshot();
    check(!f2.isNull(), "点击空白区域后仍能取帧");

    // ---- 帧可存盘 (肉眼确认用; 失败不算错) ----
    if (f2.save("virtual_device_frame.png"))
        printf("  [info] 已保存帧: virtual_device_frame.png\n");

    printf(g_fail == 0 ? "[虚拟设备] 全部通过\n" : "[虚拟设备] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
