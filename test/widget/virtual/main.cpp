// ==================== 虚拟桌面 (JZVirtualDevice) 冒烟测试 ====================
// 验证"无原生窗口也能跑一个**多窗口**桌面"这条链:
//   1) 不 show 宿主窗口时不建任何 HWND (桌面/窗口全无原生句柄) => 可无头跑 / 可进 ctest;
//   2) 桌面坐标系成立: 窗口有桌面坐标, clientToScreen / screenToClient 可逆;
//   3) 多窗口: z 序 (raise)、活动窗口 (activate / 点击激活)、按点命中 (windowAt);
//   4) 注入: 给**桌面屏幕坐标**, 自动命中窗口 + 换算客户区 + 投递控件 (点击/键盘);
//   5) 取帧: 整屏 screenshot() (桌面白屏 + 各窗口按 z 序合成) 与 screenshotWindow();
//   6) 帧可存盘供肉眼确认 (virtual_desktop_frame.png)。
//
// 注意: 本测试**不显示宿主窗口** (不建真窗口), 因此可以进 ctest;
//       要看界面的话在入口处调 desk.show() 再 app.exec() (见文末注释)。
//
// 不覆盖: 控件内部的按键语义 (由 textedit 等专项测试覆盖) —— 这里只验证"注入会被
//         正确路由到活动窗口/焦点控件"。
#include <cstdio>
#include <cstring>
#include <string>

#include "JZApplication.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZCheckbox.h"
#include "JZLineEdit.h"
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

// 取某像素是否等于给定 RGB (用于像素级验证"合成位置 == 桌面坐标")
static bool pixelIs(const JZImage& img, int x, int y, int r, int g, int b)
{
    if (img.isNull() || !img.data())
        return false;
    const uint8_t* px = img.data()->pixels();
    if (!px || x < 0 || y < 0 || x >= img.width() || y >= img.height())
        return false;
    const uint8_t* p = px + ((size_t)y * img.width() + x) * 4;
    return p[0] == (uint8_t)r && p[1] == (uint8_t)g && p[2] == (uint8_t)b;
}

// 帧里是否有非白像素 (窗口底色是白 => 非白即"有控件被画出来")
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

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲: 崩了也能看到走到哪一步

    JZApplication app;

    JZVirtualDevice desk(640, 480, "Virtual Desktop");
    // 桌面底色换成浅灰 (默认白屏): 窗口底色是白, 白对白在整屏帧里看不出窗口在哪。
    // 纯为肉眼可辨, 不影响任何坐标/语义。
    desk.setDesktopBackground(JZColor(232, 232, 232, 255));

    printf("[虚拟桌面]\n");
    check(desk.nativeHandle() == nullptr, "未 show 时不建宿主窗口 (无 HWND) => 可无头跑");
    check(desk.desktopWidth() == 640 && desk.desktopHeight() == 480, "桌面尺寸 640x480");

    // ---- 建窗口 A: 240x160 @ (40,60), 内含一个 checkbox ----
    // 注: 子控件必须是堆对象 —— 窗口析构时会 delete 自己的子控件 (JZObject 父子所有权),
    //     栈对象挂上去会在关窗时被 delete 而崩。
    JZVirtualWindow* wa = desk.createWindow(240, 160, "win-a", 40, 60);
    JZCheckbox* box = new JZCheckbox(wa);
    box->setText("power");
    box->setGeometry(20, 20, 200, 40);
    box->show();
    wa->renderNow();

    check(wa->nativeHandle() == nullptr, "桌面内的窗口也没有原生窗口");
    check(wa->screenX() == 40 && wa->screenY() == 60, "窗口有桌面坐标 (40,60)");
    check(wa->desktop() == &desk, "窗口知道自己属于哪个桌面");

    // ---- 坐标换算 (有桌面坐标系, 这些才有意义) ----
    const JZPoint sp = desk.clientToScreen(wa, 10, 20);
    check(sp.x() == 50 && sp.y() == 80, "clientToScreen: 客户区(10,20) -> 桌面(50,80)");
    const JZPoint cp = desk.screenToClient(wa, 50, 80);
    check(cp.x() == 10 && cp.y() == 20, "screenToClient 与上互逆");

    // ---- 取帧 ----
    const JZImage full = desk.screenshot();
    check(full.width() == 640 && full.height() == 480, "整屏截图 = 桌面尺寸");
    // 像素级验证"合成 + 坐标": 桌面空白处是底色, 窗口矩形内是窗口底色 (白)
    check(pixelIs(full, 10, 10, 232, 232, 232), "桌面空白处 = 桌面底色");
    check(pixelIs(full, 160, 140, 255, 255, 255), "窗口 A 的桌面坐标区域被合成成窗口底色");
    const JZImage winShot = desk.screenshotWindow(wa);
    check(winShot.width() == 240 && winShot.height() == 160, "单窗口截图 = 窗口尺寸");
    check(hasInk(winShot), "单窗口帧里有控件");

    // ---- 命中测试 ----
    check(desk.windowAt(10, 10) == nullptr, "窗口外(桌面白屏)不命中任何窗口");
    check(!desk.injectClickAt(10, 10), "点桌面白屏不投递给任何窗口");
    check(desk.windowAt(50, 80) == wa, "窗口内命中该窗口");

    // ---- 用桌面坐标点击 checkbox: 桌面(90,100) -> A 客户区(50,40) ----
    check(!box->isChecked(), "checkbox 初始未勾选");
    desk.injectClickAt(90, 100);
    check(box->isChecked(), "桌面坐标点击 -> 命中窗口 -> 换算客户区 -> 控件状态变化");

    // ---- 多窗口: z 序 / 活动窗口 / 重叠命中 ----
    JZVirtualWindow* wb = desk.createWindow(240, 160, "win-b", 100, 120);   // 与 A 重叠
    check(desk.windows().size() == 2, "桌面上有两个窗口");
    check(desk.activeWindow() == wb, "新建窗口成为活动窗口");
    check(desk.windowAt(120, 140) == wb, "重叠处命中 z 序最上的窗口 B");

    desk.raise(wa);
    check(desk.windowAt(120, 140) == wa, "raise(A) 后重叠处命中 A");
    check(desk.activeWindow() == wb, "raise 只改 z 序, 不改活动窗口");

    desk.activate(wa);
    check(desk.activeWindow() == wa, "activate(A) 切换活动窗口");

    desk.moveWindow(wa, 300, 300);
    check(desk.windowAt(120, 140) == wb, "移走 A 后原重叠点回落到 B");
    {
        const JZImage moved = desk.screenshot();
        check(pixelIs(moved, 60, 70, 232, 232, 232), "A 原桌面坐标处回到桌面底色 (合成跟着位置走)");
        check(pixelIs(moved, 400, 400, 255, 255, 255), "A 的新桌面坐标处出现窗口");
    }

    desk.injectClickAt(150, 150);        // 落在 B 的客户区
    check(desk.activeWindow() == wb, "点击某窗口的客户区 => 该窗口被激活");

    // ---- 键盘注入: 路由到活动窗口的焦点控件 ----
    JZVirtualWindow* wc = desk.createWindow(240, 160, "win-c", 0, 0);
    JZLineEdit* edit = new JZLineEdit(wc);
    edit->setGeometry(10, 10, 200, 30);
    edit->show();
    wc->renderNow();
    check(desk.activeWindow() == wc, "新窗口 C 为活动窗口");

    desk.injectClickAt(20, 20);          // 桌面(20,20) -> C 客户区(20,20): 输入框上
    check(desk.injectText("ab"), "injectText 把字符投给活动窗口");
    check(edit->text() == "ab", "键盘注入落到焦点控件 (lineEdit 文本 == ab)");

    // ---- 标题查找 (对齐 CU 的 titleFilter) ----
    check(desk.byTitle("win-b") == wb, "按标题片段能找到窗口");
    check(desk.byTitle("nope") == nullptr, "找不到时返回 nullptr");

    // ---- 关窗 ----
    desk.destroyWindow(wc);
    check(desk.windows().size() == 2, "关掉 C 后剩两个窗口");
    check(desk.activeWindow() == wb, "活动窗口退到最上层那个");

    // ---- 存帧 (肉眼确认; 失败不算错) ----
    if (desk.screenshot().save("virtual_desktop_frame.png"))
        printf("  [info] 已保存整屏帧: virtual_desktop_frame.png\n");

    printf(g_fail == 0 ? "[虚拟桌面] 全部通过\n" : "[虚拟桌面] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
