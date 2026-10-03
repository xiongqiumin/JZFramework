// ==================== 虚拟桌面 (JZVirtualDevice) 冒烟测试 ====================
// 验证"无原生窗口也能跑一个**多窗口**桌面"这条链, 以及窗口管理器那半边:
//   1) 不 show 宿主窗口时不建任何 HWND (桌面/窗口全无原生窗口) => 可无头跑 / 可进 ctest;
//   2) 虚拟窗口**有句柄**: nativeHandle() == 虚拟侧的窗口对象 (不再恒为 nullptr);
//      两个平台都有"平台窗口", 只是一个是 HWND, 一个由虚拟桌面管理器持有;
//   3) 客户区就是一个真 JZWindow: 控件挂它下面, 渲染/命中/注入与真实窗口同一条链;
//   4) 装饰: 标题栏 + 边框 + 最小化/最大化/关闭; 拖动标题栏移动窗口; 点关闭关掉窗口;
//   5) 坐标系成立: 客户区 <-> 桌面可逆; mapToGlobal 与桌面坐标同口径;
//   6) 注入: 给**桌面坐标**, 自动命中窗口(按 z 序) + 换算客户区 + 投递控件;
//   7) 取帧: 整屏 screenshot() (底色 + 各窗口按 z 序合成) 与 screenshotWindow();
//   8) 帧可存盘供肉眼确认 (virtual_desktop_frame.png)。
//
// 注意: 本测试**不显示宿主窗口** (不建真窗口), 因此可以进 ctest。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "JZApplication.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZWindow.h"
#include "JZCheckbox.h"
#include "JZLineEdit.h"
#include "JZPushButton.h"
#include "JZImage.h"
#include "JZJSEngine.h"        // JS 侧: 引擎里 new 出来的窗口也该是虚拟的
#include "JZJSWidgetModule.h"  // 控件族全局类 (JZWindow/JZPushButton/...)
#include "JZMainWindow.h"      // 宿主界面 (MainWindow + ScrollArea + 屏幕视图)
#include "JZScrollArea.h"
#include "JZVirtualScreenView.h"
#include "JZVirtualProcess.h"  // 虚拟进程: 一个窗口一个线程 + 一份 JS 运行时
#include "JZThread.h"

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

// 语义树里按角色找节点 (与 CU 的布局通道同一条路; 找按钮用)
static bool findRole(const Json::Value& n, const std::string& role, Json::Value& out)
{
    if (!n.isObject())
        return false;
    if (n.isMember("className") && n["className"].asString() == role) {
        out = n;
        return true;
    }
    const Json::Value& kids = n["children"];
    if (kids.isArray()) {
        for (const auto& c : kids) {
            if (findRole(c, role, out))
                return true;
        }
    }
    return false;
}

// 标题栏第 i 个按钮 (0=最小化 1=最大化 2=关闭) 的**中心**桌面坐标 —— 与窗口管理器的
// 布局公式一致 (测试要"点得准", 就得按同一套常量算)。
static JZPoint buttonCenter(const JZVirtualWindow* w, int index)
{
    const int fw = w->frameWidth();
    const int right = w->frameX() + fw - JZVirtualWindow::kBorder - 4;
    const int x = right - (2 - index) * (JZVirtualWindow::kButton + JZVirtualWindow::kButtonGap)
                  - JZVirtualWindow::kButton / 2;
    const int y = w->frameY() + JZVirtualWindow::kTitleBar / 2;
    return JZPoint(x, y);
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲: 崩了也能看到走到哪一步

    JZApplication app;

    JZVirtualDevice desk(640, 480);
    // 桌面底色换成浅灰 (默认白屏): 窗口底色是白, 白对白在整屏帧里看不出窗口在哪。
    // 纯为肉眼可辨, 不影响任何坐标/语义。
    desk.setDesktopBackground(JZColor(232, 232, 232, 255));

    printf("[虚拟桌面]\n");
    check(desk.desktopWidth() == 640 && desk.desktopHeight() == 480, "桌面尺寸 640x480");
    check(desk.activeWindow() == nullptr, "还没有窗口时活动窗口为空");

    // ---- 建窗口 A: 客户区 240x160 @ 外框(40,60), 内含一个 checkbox ----
    // 注: 子控件必须是堆对象 —— 窗口析构时会 delete 自己的子控件 (JZObject 父子所有权),
    //     栈对象挂上去会在关窗时被 delete 而崩。
    JZVirtualWindow* wa = desk.createWindow(240, 160, "win-a", 40, 60);
    JZCheckbox* box = new JZCheckbox(wa->client());     // 控件挂在**客户区**那个 JZWindow 上
    box->setText("power");
    box->setGeometry(20, 20, 200, 40);
    box->show();
    wa->renderNow();

    check(wa->screen() == &desk, "窗口知道自己属于哪块桌面");
    check(wa->client() != nullptr && wa->client()->isTopLevelWindow() == false,
          "客户区是真 JZWindow, 但它是**内部窗口**(不登记进 app 顶层窗口表)");
    check(jzApp->topLevelWindows().empty(), "虚拟窗口不出现在 JZApplication 的顶层窗口表里");
    check(wa->nativeHandle() != nullptr, "虚拟窗口**有**平台句柄 (不再恒为 nullptr)");
    check(wa->nativeHandle() == (void*)wa, "句柄就是虚拟侧那个窗口对象");

    // ---- 外框几何: 外框 = 客户区 + 四周 2px 边框 + 顶部 22px 标题栏 ----
    {
        const int kBorder = JZVirtualWindow::kBorder;
        const int kTitle = JZVirtualWindow::kTitleBar;
        check(wa->frameX() == 40 && wa->frameY() == 60, "外框在桌面 (40,60)");
        check(wa->frameWidth() == 240 + 2 * kBorder, "外框宽 = 客户区宽 + 2*边框");
        check(wa->frameHeight() == kTitle + 160 + kBorder, "外框高 = 标题栏 + 客户区高 + 边框");
        check(wa->clientX() == 40 + kBorder && wa->clientY() == 60 + kTitle,
              "客户区原点 = 外框原点 + 装饰尺寸");
    }

    // ---- 坐标换算 (有桌面坐标系, 这些才有意义) ----
    check(wa->clientToScreen(JZPoint(0, 0)) == JZPoint(wa->clientX(), wa->clientY()),
          "clientToScreen: 客户区原点 -> 桌面客户区原点");
    check(wa->screenToClient(JZPoint(wa->clientX() + 10, wa->clientY() + 20)) == JZPoint(10, 20),
          "screenToClient 与 clientToScreen 互逆");

    // mapToGlobal: 虚拟窗口下也必须是**桌面坐标** (口径由窗口后端的 clientToScreen 给出)
    {
        const JZPoint boxTL = box->mapToGlobal(JZPoint(0, 0));
        check(boxTL.x() == wa->clientX() + 20 && boxTL.y() == wa->clientY() + 20,
              "mapToGlobal(控件) = 窗口客户区桌面原点 + 控件客户区坐标");
        const JZImage grabbed = box->grab();
        check(grabbed.width() == box->width() && grabbed.height() == box->height(),
              "widget->grab() 在虚拟窗口上可用 (源帧来自窗口后端, 不是抓屏)");
    }

    // ---- 取帧: 底色 / 标题栏 / 客户区 ----
    const JZImage full = desk.screenshot();
    check(full.width() == 640 && full.height() == 480, "整屏截图 = 桌面尺寸");
    check(pixelIs(full, 10, 10, 232, 232, 232), "桌面空白处 = 桌面底色");
    check(pixelIs(full, 200, 200, 255, 255, 255), "窗口客户区被合成在它自己的桌面位置上");
    check(!pixelIs(full, 40 + 100, 60 + 10, 232, 232, 232), "标题栏画出来了 (不是桌面底色)");
    check(hasInk(desk.screenshotWindow(wa)), "单窗口帧里有控件");
    check(desk.screenshotWindow(wa).width() == 240, "单窗口截图 = 客户区尺寸");

    // ---- DPI: 与主机共享缩放 (scale 只改渲染分辨率; 坐标/命中/注入一律逻辑) ----
    {
        desk.setScale(2.0f);
        check(desk.scale() == 2.0f, "setScale 记住缩放");
        check(desk.desktopWidth() == 640 && desk.desktopHeight() == 480,
              "缩放不进桌面逻辑尺寸 (坐标仍 640x480)");
        const JZImage hi = desk.screenshot();
        check(hi.width() == 1280 && hi.height() == 960,
              "整屏帧 = 桌面逻辑尺寸 × 缩放 (物理像素, 同 JZWidget::grab 约定)");
        check(pixelIs(hi, 20, 20, 232, 232, 232), "缩放后桌面底色照旧");
        check(pixelIs(hi, 400, 400, 255, 255, 255),
              "窗口客户区仍合成在自己的桌面上 (同一坐标 × 缩放)");
        check(desk.screenshotWindow(wa).width() == 480, "单窗口帧 = 客户区逻辑尺寸 × 缩放");
        check(wa->frame().width() == 480 && wa->client()->device()->scale() == 2.0f,
              "客户区按桌面缩放渲染 (帧缓冲 = 逻辑尺寸 × 缩放)");
        // 缩放**不进坐标**: 命中/换算/注入还是那一套逻辑坐标
        check(desk.windowAt(wa->clientX() + 10, wa->clientY() + 10) == wa,
              "命中测试仍按逻辑坐标 (与缩放无关)");
        check(wa->clientToScreen(JZPoint(10, 20)) ==
                  JZPoint(wa->clientX() + 10, wa->clientY() + 20),
              "坐标换算不受缩放影响");
        desk.setScale(1.0f);
        check(desk.screenshot().width() == 640 && wa->frame().width() == 240,
              "缩放改回 1: 帧缓冲回到逻辑尺寸");
    }

    // ---- 命中测试 ----
    check(desk.windowAt(10, 10) == nullptr, "窗口外(桌面空白)不命中任何窗口");
    check(!desk.injectClickAt(10, 10), "点桌面空白不投递给任何窗口");
    check(desk.windowAt(wa->clientX() + 10, wa->clientY() + 10) == wa, "窗口内命中该窗口");

    // ---- 用桌面坐标点击 checkbox ----
    check(!box->isChecked(), "checkbox 初始未勾选");
    {
        const JZPoint c = box->mapToGlobal(JZPoint(box->width() / 2, box->height() / 2));
        desk.injectClickAt(c.x(), c.y());
    }
    check(box->isChecked(), "桌面坐标点击 -> 命中窗口 -> 换算客户区 -> 控件状态变化");

    // ---- 多窗口: z 序 / 活动窗口 / 重叠命中 ----
    JZVirtualWindow* wb = desk.createWindow(240, 160, "win-b", 100, 120);   // 与 A 重叠
    check(desk.windows().size() == 2, "桌面上有两个窗口");
    check(desk.activeWindow() == wb, "新建窗口成为活动窗口");
    check(desk.windowAt(150, 180) == wb, "重叠处命中 z 序最上的窗口 B");

    desk.raise(wa);
    check(desk.windowAt(150, 180) == wa, "raise(A) 后重叠处命中 A");
    check(desk.activeWindow() == wb, "raise 只改 z 序, 不改活动窗口");

    desk.activate(wa);
    check(desk.activeWindow() == wa, "activate(A) 切换活动窗口");
    check(wa->isActive() && !wb->isActive(), "活动窗口的客户区拿到键盘焦点");

    // ---- 窗口管理器: 拖动标题栏移动窗口 ----
    {
        const int fx0 = wa->frameX(), fy0 = wa->frameY();
        const int tx = wa->frameX() + 100, ty = wa->frameY() + 10;   // 标题栏上 (避开按钮)
        check(!desk.isDragging(), "初始不在拖动中");
        desk.injectMouseAtScreen(JZMouseEvent::Press, JZMouseEvent::LeftButton, tx, ty);
        check(desk.isDragging(), "按标题栏 -> 进入拖动");
        desk.injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton, tx + 60, ty + 40);
        desk.injectMouseAtScreen(JZMouseEvent::Release, JZMouseEvent::LeftButton, tx + 60, ty + 40);
        check(!desk.isDragging(), "松开 -> 结束拖动");
        check(wa->frameX() == fx0 + 60 && wa->frameY() == fy0 + 40, "拖动把窗口移到新位置");
        check(wa->clientX() == wa->frameX() + JZVirtualWindow::kBorder,
              "客户区跟着外框走 (坐标口径只有一处)");
    }

    // ---- 窗口管理器: 边框拖动缩放 ----
    {
        const int cw0 = wa->client()->width();
        const int rx = wa->frameX() + wa->frameWidth() - 1;   // 右边框
        const int ry = wa->frameY() + wa->frameHeight() / 2;
        desk.injectMouseAtScreen(JZMouseEvent::Press, JZMouseEvent::LeftButton, rx, ry);
        desk.injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton, rx + 40, ry);
        desk.injectMouseAtScreen(JZMouseEvent::Release, JZMouseEvent::LeftButton, rx + 40, ry);
        check(wa->client()->width() == cw0 + 40, "拖右边框 -> 客户区变宽 40");
    }

    // 任务栏按钮: 顺序 = 窗口**诞生顺序** (JZVirtualWindow::creationSeq), 与 z 序无关 ——
    // 激活/置顶一个窗口**不会**让按钮换位置 (2026-10-03 改; 以前按 z 序, 点一下按钮整排就换位)。
    // 两段用例(最小化还原 / 点按钮激活)都用它算某个窗口的按钮下标。
    auto taskButtonIndexOf = [&](JZVirtualWindow* target) {
        std::vector<JZVirtualWindow*> v;
        for (auto* x : desk.windows()) {
            if (x->isVisible())
                v.push_back(x);
        }
        std::sort(v.begin(), v.end(), [](JZVirtualWindow* a, JZVirtualWindow* b) {
            return a->creationSeq() < b->creationSeq();
        });
        for (int i = 0; i < (int)v.size(); ++i) {
            if (v[i] == target)
                return i;
        }
        return -1;
    };

    // ---- 窗口管理器: 最小化 / 最大化 / 关闭 ----
    {
        const JZPoint minC = buttonCenter(wa, 0);
        desk.injectClickAt(minC.x(), minC.y());
        check(wa->isMinimized(), "点最小化按钮 -> 窗口最小化");
        check(wa->frameHeight() == JZVirtualWindow::kTitleBar, "最小化后外框只剩标题栏");
        // ⚠ 最小化 = 桌面上**没有它** (不合成、不命中) —— 同真实 Windows: 只剩任务栏那颗按钮
        const int idx = taskButtonIndexOf(wa);
        check(idx >= 0, "任务栏上有它的按钮");
        check(desk.windowAt(minC.x(), minC.y()) == nullptr,
              "最小化后在原位点不到它 (桌面不留标题栏)");
        const JZRect tb0 = desk.taskButtonRect(idx);
        desk.injectClickAt(tb0.left() + 4, tb0.top() + 4);
        check(!wa->isMinimized(), "点任务栏按钮 -> 还原它");
        check(desk.windowAt(minC.x(), minC.y()) == wa, "还原后原位又能点中它");

        const JZRect before = wa->frameGeometry();
        const JZPoint maxC = buttonCenter(wa, 1);
        desk.injectClickAt(maxC.x(), maxC.y());
        check(wa->isMaximized(), "点最大化按钮 -> 铺满工作区");
        check(wa->frameGeometry() == desk.workArea(),
              "最大化后外框 = 工作区 (桌面减掉任务栏)");
        // 任务栏是 chrome: 最大化也盖不住它 (像素级确认任务栏还在; 取按钮区右边的空白处)
        check(pixelIs(desk.screenshot(), desk.desktopWidth() - 10, desk.taskbarTop() + 6,
                      192, 192, 192),
              "最大化后任务栏仍画在最上面 (没被窗口盖住)");
        check(wa->frameY() + wa->frameHeight() <= desk.taskbarTop(),
              "最大化窗口的下边界在任务栏之上");
        // 最大化后按钮位置跟着外框变 -> 按新位置再点一次 (按钮在右上角, 与真窗口一致)
        const JZPoint maxC2 = buttonCenter(wa, 1);
        desk.injectClickAt(maxC2.x(), maxC2.y());
        check(!wa->isMaximized(), "再点一次还原");
        check(wa->frameGeometry() == before, "还原回最大化前的窗口矩形");
    }

    // ---- 底部任务栏 + 开始菜单 (桌面 chrome) ----
    {
        check(desk.taskbarTop() == desk.desktopHeight() - JZVirtualDevice::kTaskbarHeight,
              "任务栏在桌面最底下一条");
        check(desk.workArea() == JZRect(0, 0, desk.desktopWidth(), desk.taskbarTop()),
              "工作区 = 桌面减掉任务栏");

        // 任务栏那一条上没有窗口 (即使有窗口摆在那儿: 往后拖的窗口可以探进任务栏底下)
        const int barY = desk.taskbarTop() + 6;
        check(desk.windowAt(desk.taskbarTop() - 6, barY) == nullptr, "任务栏区域不命中窗口");

        // 每个**可见**窗口一个按钮; 点非活动窗口的按钮 = 激活它
        std::vector<JZVirtualWindow*> vis;
        for (auto* w : desk.windows()) {
            if (w->isVisible())
                vis.push_back(w);
        }
        check(vis.size() >= 2, "任务栏上有多个窗口按钮");
        JZVirtualWindow* other = vis.front();
        for (auto* w : vis) {
            if (w != desk.activeWindow()) {
                other = w;
                break;
            }
        }
        // 按钮顺序 = 窗口诞生顺序 (见上面的 taskButtonIndexOf) ⇒ 激活**不会**让按钮换位置
        auto buttonRectOf = [&](JZVirtualWindow* target) {
            const int i = taskButtonIndexOf(target);
            return i >= 0 ? desk.taskButtonRect(i) : JZRect(0, 0, 0, 0);
        };
        const JZRect tb = buttonRectOf(other);
        check(!tb.isEmpty() && tb.top() >= desk.taskbarTop(), "窗口按钮排在任务栏里");
        desk.injectClickAt(tb.left() + 4, tb.top() + 4);
        check(desk.activeWindow() == other, "点任务栏上的窗口按钮 -> 激活那个窗口");
        // ⚠ 再点**同一个位置**: 顺序稳定 ⇒ 还是它 (以前按 z 序排按钮, 这一下会点到别的窗口)
        desk.injectClickAt(tb.left() + 4, tb.top() + 4);
        check(desk.activeWindow() == other, "再点同一位置仍是它 (按钮顺序不随激活重排)");

        // 最小化后点它的按钮 = 还原 (同真实任务栏)
        other->setMinimized(true);
        const JZRect tb2 = buttonRectOf(other);
        desk.injectClickAt(tb2.left() + 4, tb2.top() + 4);
        check(!other->isMinimized(), "点最小化窗口的任务栏按钮 -> 还原它");

        // 开始按钮 -> 开始菜单 (菜单项由宿主登记; 回调在这里数一下)
        int clicks = 0;
        desk.addStartMenuItem("测试项", [&clicks]() { ++clicks; });
        const JZRect sb = desk.startButtonRect();
        check(sb.intersects(desk.workArea()) == false, "开始按钮在任务栏上 (工作区之外)");
        desk.injectClickAt(sb.left() + 4, sb.top() + 4);
        check(desk.isStartMenuOpen(), "点开始按钮 -> 弹出开始菜单");
        // 像素级: 菜单面板真的画在桌面上 (取面板右边距, 免得撞上高亮的菜单项)
        check(pixelIs(desk.screenshot(), desk.startMenuRect().right() - 2,
                      desk.startMenuRect().top() + 8, 192, 192, 192),
              "开始菜单面板画在桌面上");
        const JZRect it0 = desk.startMenuItemRect(0);
        desk.injectClickAt(it0.left() + 4, it0.top() + 4);
        check(clicks == 1, "点开始菜单项 -> 回调被执行");
        check(!desk.isStartMenuOpen(), "执行后菜单收起");

        // 菜单开着时点别处: 只收起菜单, 这一下不落到下面的窗口上
        desk.injectClickAt(sb.left() + 4, sb.top() + 4);
        check(desk.isStartMenuOpen(), "再点开始按钮 -> 又弹出来");
        JZVirtualWindow* wasActive = desk.activeWindow();
        desk.injectClickAt(desk.desktopWidth() - 4, 4);      // 桌面右上角 (无窗口处)
        check(!desk.isStartMenuOpen(), "点空白处 -> 菜单收起");
        check(desk.activeWindow() == wasActive, "收起菜单那一下不误触窗口");
    }

    // ---- 键盘注入: 路由到活动窗口的焦点控件 ----
    JZVirtualWindow* wc = desk.createWindow(240, 160, "win-c", 20, 20);
    JZLineEdit* edit = new JZLineEdit(wc->client());
    edit->setGeometry(10, 10, 200, 30);
    edit->show();
    wc->renderNow();
    check(desk.activeWindow() == wc, "新窗口 C 为活动窗口");

    {
        const JZPoint e = edit->mapToGlobal(JZPoint(10, 10));
        desk.injectClickAt(e.x(), e.y());       // 点输入框 (桌面坐标)
    }
    check(desk.injectText("ab"), "injectText 把字符投给活动窗口");
    check(edit->text() == "ab", "键盘注入落到焦点控件 (lineEdit 文本 == ab)");

    // ---- 标题查找 (对齐 CU 的 titleFilter) ----
    check(desk.byTitle("win-b") == wb, "按标题片段能找到窗口");
    check(desk.byTitle("nope") == nullptr, "找不到时返回 nullptr");

    // ---- 点关闭按钮: 窗口从桌面上消失 (销毁), 活动窗口退回最上层那个 ----
    {
        const JZPoint closeC = buttonCenter(wc, 2);
        desk.injectClickAt(closeC.x(), closeC.y());
        check(desk.windows().size() == 2, "点关闭按钮 -> 窗口被关掉并从桌面移除");
        check(desk.byTitle("win-c") == nullptr, "关掉的窗口查不到了");
        check(desk.activeWindow() == desk.windows().back(),
              "活动窗口退到 z 序最上层那个");
    }

    // ---- 关窗 API + close() 的框架语义 ----
    // close() == PostMessage(hwnd, WM_CLOSE): 派发 closeEvent (应用可 ignore) → 接受才 hide +
    // **从桌面摘下来** (detachWindow; 客户区对象保留) ⇒ 它不再出现/命中/被查, 而对象还在。
    {
        JZVirtualWindow* wd = desk.createWindow(120, 80, "win-d", 400, 20);
        check(desk.windows().size() == 3, "新建 D 后三个窗口");
        // wd 的客户区坐标 = (外框原点 + 装饰); 取它客户区里一点验证命中
        check(desk.windowAt(wd->clientX() + 5, wd->clientY() + 5) == wd, "D 的客户区可命中");
        check(wd->close(), "客户区 close() 被接受 (closeEvent 默认 accept)");
        check(!wd->isVisible(), "close() 后窗口已隐藏");
        check(desk.windowAt(wd->clientX() + 5, wd->clientY() + 5) == nullptr,
              "隐藏的窗口不再被命中 (合成按可见性跳过)");
        check(desk.byTitle("win-d") == nullptr, "关掉的窗口不再出现在窗口表里 (已从桌面摘下来)");
        // 对象还在 ⇒ 再 show() 就是"重建 HWND": 同一台窗口、同一个客户区挂回桌面
        wd->show();
        check(desk.byTitle("win-d") == wd, "关掉后 show() 又回到桌面上 (客户区是同一个对象)");
        check(desk.windowAt(wd->clientX() + 5, wd->clientY() + 5) == wd, "回来之后又能被命中");
        desk.destroyWindow(wd);       // 这次是**硬删** (收摊那条路)
        check(desk.windows().size() == 2, "销毁 D 后回到两个窗口");
        check(desk.byTitle("win-d") == nullptr, "硬删之后彻查也找不到");
    }

    // ==================== JS 侧: 引擎里 new 出来的窗口也是虚拟的 ====================
    // 一句 engine.setWindowHost(&desk) (等价进程级的 jzSetWindowHost), 之后 JS 里
    // `new JZWindow(...)` 建出来的**不是真窗口**, 而是这块桌面上的虚拟窗口 ——
    // JS 侧一行都不用改 (这就是"同一份脚本能在虚拟机上确定性回放"的入口)。
    {
        JZJSEngine engine;
        engine.setWorkDir(JZ_JS_WRAPPER_DIR);
        engine.setLibraryDir({ std::string(JZ_JS_WRAPPER_DIR) });   // widget/dist 靠它定位
        engine.setWindowHost(&desk);        // ← 就这一句: 窗口后端交给虚拟桌面
        engine.init();
        JZJSWidgetModule widgetModule;      // 控件族全局类 (JZWindow / JZPushButton / ...)
        widgetModule.initEngine(&engine);

        const size_t before = desk.windows().size();
        JZJSValuePtr r = engine.eval(
            "globalThis.__vw = new JZWindow(320, 200, 'js-win');"
            "const b = new JZPushButton('Add');"              // 构造形态: (text, parent?) 
            "b.setParentWidget(globalThis.__vw);"             // 挂到窗口客户区(JS 侧登记父子/保活)
            "b.setGeometry(0, 0, 140, 32);"
            "b.show();"
            "globalThis.__vclicked = 0;"
            "b.onClicked = () => { globalThis.__vclicked += 1; };"
            "globalThis.__vw.show();"
            "'ok';");
        check(engine.fromJSValue<std::string>(r.get()) == "ok",
              "JS: new JZWindow + new JZPushButton + show() 跑通");

        JZVirtualWindow* jw = desk.byTitle("js-win");
        check(jw != nullptr && desk.windows().size() == before + 1,
              "JS 里 new 出来的窗口出现在虚拟桌面上 (没有建真窗口)");
        if (jw)
        {
            check(jw->client() != nullptr && jw->client()->nativeHandle() == jw,
                  "JS 窗口的 nativeHandle() = 虚拟侧的窗口对象");
            check(desk.activeWindow() == jw, "JS show() 后它成为活动窗口 (后端 show 里置顶+聚焦)");
            check(jw->clientX() == jw->frameX() + JZVirtualWindow::kBorder &&
                  jw->client()->width() == 320 && jw->client()->height() == 200,
                  "JS 给的 320x200 就是客户区尺寸 (装饰在外框上, 不占客户区)");

            // 语义树里找按钮 (AI 看到的那份树), 用它的屏幕坐标点它
            Json::Value tree = jw->client()->dumpSemanticTree(-1);
            Json::Value btn;
            check(findRole(tree, "button", btn), "JS 建的按钮出现在语义树里 (AI 能看见)");
            if (findRole(tree, "button", btn))
            {
                const Json::Value& bd = btn["boundsInScreen"];
                const int cx = bd[0].asInt() + bd[2].asInt() / 2;
                const int cy = bd[1].asInt() + bd[3].asInt() / 2;
                desk.injectClickAt(cx, cy);
                JZJSValuePtr n = engine.eval("globalThis.__vclicked");
                check(engine.fromJSValue<int>(n.get()) == 1,
                      "在虚拟桌面上点击 -> 落到 JS 回调 (JS / 桌面 / 控件同一条链)");
            }
        }

        check(engine.windowHost() == &desk, "引擎记住了窗口宿主");
    }
    check(jzWindowHost() == nullptr, "引擎析构后窗口宿主自动还原 (之后新建的又是真窗口)");

    // ==================== 虚拟进程: 一个窗口一个线程 ====================
    // JZVirtualProcess = 一个 JZThread + 一个 JZEngineRuntime(内含 JZJSEngine)。它里面跑的
    // JS 建出来的窗口就属于那个线程/那份引擎; 与桌面的通信: 建窗/状态走锁, 注入输入走
    // jzApp->sendEvent 同步投递, 帧由进程线程推给桌面 (见 JZVirtualProcess.h 的头注释)。
    {
        const size_t beforeProc = desk.windows().size();
        JZVirtualProcess* proc = desk.createProcess("app-js",
            "globalThis.__pw = new JZWindow(300, 180, 'proc-win');"
            "const b = new JZPushButton('Proc');"
            "b.setParentWidget(globalThis.__pw);"
            "b.setGeometry(0, 0, 120, 30);"
            "b.show();"
            "b.onClicked = () => { globalThis.__pw.setTitle('proc-clicked'); };"
            "globalThis.__pw.show();");
        check(proc != nullptr, "建进程 (一个线程 + 一份 JS 运行时 + 引擎)");
        if (proc)
        {
            check(proc->processId() > 0 && proc->name() == "app-js", "进程有 id / 名字");
            check(proc->waitWindow(8000), "进程里 JS 建的窗口出现了 (在它自己的线程上)");
            // ⚠ waitWindow 只保证"可见" —— 出首帧/激活都排在它后面、都由**进程线程**做 (见
            //   JZVirtualProcess.h 那段)。若主线程这时立刻跟它做跨线程同步调用 (注入输入/取帧),
            //   两边会互等: 实测进程线程卡在 show 里的 setFocus(给主线程窗口发焦点事件)、
            //   主线程卡在 injectMouse 的投递上, 双双不动。所以等它"show 全做完"再动手。
            for (int i = 0; i < 800 && proc->window() && !proc->window()->isReadyForUse(); ++i)
                JZThread::msleep(10);
            check(proc->window() && proc->window()->isReadyForUse(),
                  "进程窗口 show 全部做完 (可以跨线程动手了)");

            JZVirtualWindow* pw = proc->window();
            check(pw != nullptr && pw->process() == proc, "窗口归属这个进程");
            check(pw->ownerThread() == proc && pw->ownerThread() != jzApp->mainThread(),
                  "窗口的线程 = 进程线程 (不是主线程)");
            check(desk.activeWindow() == pw, "进程窗口 show() 后成为桌面的活动窗口");
            check(!pw->frame().isNull() && pw->frame().width() == 300,
                  "桌面拿到了进程推上来的帧 (跨线程合成, 不阻塞)");

            // 跨线程点击: 主线程 -> 桌面(锁内路由) -> 投到进程线程 -> JS 回调
            Json::Value tree = pw->client()->dumpSemanticTree(-1);
            Json::Value btn;
            check(findRole(tree, "button", btn), "进程窗口里的按钮出现在语义树里 (跨线程读)");
            if (findRole(tree, "button", btn))
            {
                const Json::Value& bd = btn["boundsInScreen"];
                const int cx = bd[0].asInt() + bd[2].asInt() / 2;
                const int cy = bd[1].asInt() + bd[3].asInt() / 2;
                check(desk.injectClickAt(cx, cy), "在桌面上点进程窗口 (跨线程投递输入)");
                // JS 回调改了窗口标题 -> 从桌面这边就能看见 (证明回调真在它的线程上跑了)
                for (int i = 0; i < 200 && !desk.byTitle("proc-clicked"); ++i)
                    JZThread::msleep(10);
                check(desk.byTitle("proc-clicked") == pw,
                      "点击落到进程里的 JS 回调 (窗口标题被改)");
            }

            // ---- 点关闭按钮: 进程窗口要由**它自己的线程**收掉 ----
            // 客户区 (那个 JZWindow) 是进程线程里的对象: 在主线程 delete 它会踩
            // JZObject::clearGuardPointer 的线程亲和断言 (以前点这儿直接崩 + 桌面状态坏掉)。
            // 现在 destroyWindow 把"关窗"整件事投到窗口自己的线程 -> 关得掉, 且留给它的线程
            // (客户区由那个线程的那份引擎/桌面逻辑收尾)。
            {
                const JZPoint closeC = buttonCenter(pw, 2);
                check(desk.injectClickAt(closeC.x(), closeC.y()), "点进程窗口的关闭按钮");
                // 进程窗口的关窗是**异步**的 (整个关窗投到窗口自己的线程: 客户区只有那个线程
                // 能销毁; 在那边同步等它就是互等), 所以这里等它被收走 —— 判据用窗口数
                // (窗口标题刚才被 JS 改成了 proc-clicked, 不能拿 proc-win 当判据)。
                for (int i = 0; i < 500 && desk.windows().size() > beforeProc; ++i)
                    JZThread::msleep(10);
                check(desk.byTitle("proc-win") == nullptr && desk.byTitle("proc-clicked") == nullptr,
                      "进程窗口被关掉并从桌面移除 (销毁发生在它自己的线程上)");
                check(desk.windows().size() == beforeProc, "关掉进程窗口后桌面回到起进程前的窗口数");
                check(!desk.processes().empty(), "关窗口 ≠ 停程序 (进程还在程序表里)");
            }

            // 停进程: 线程结束 + 它的窗口一起从桌面收走
            desk.terminateProcess(proc);
            check(desk.processes().empty(), "terminateProcess: 进程从桌面摘掉");
            check(desk.byTitle("proc-win") == nullptr && desk.byTitle("proc-clicked") == nullptr,
                  "进程的窗口被一起收走");
            // ⚠ 期望值是 beforeProc（= 起进程**之前**的窗口数）：进程那个窗口已经在
            //   terminateProcess 里被收走（上一条 byTitle==nullptr 就证明了），所以数量该**回退**。
            //   原来写的 +1 是照抄了上面 js-win 那条（那条窗口是保留的，+1 才对）。
            check(desk.windows().size() == beforeProc, "进程的窗口收走后桌面回到原来的窗口数");
        }
    }

    // ---- JZScrollArea: 内容对齐 (Qt QScrollArea::setAlignment 语义) + 底色走样式表 ----
    // (虚拟桌面的宿主就靠这两条: 屏幕在视口里居中 + 余下区域刷黑)
    {
        JZScrollArea* area = new JZScrollArea();
        area->resize(200, 100);
        JZWidget* content = new JZWidget();
        content->resize(60, 40);
        area->setWidget(content);        // setWidget 内部会摆一次 (默认左上)
        check(content->x() == 0 && content->y() == 0, "默认对齐 = 左上 (老行为不变)");

        area->setAlignment(JZScrollArea::AlignHCenter | JZScrollArea::AlignVCenter);
        check(content->x() == (200 - 60) / 2 && content->y() == (100 - 40) / 2,
              "视口比内容大 -> 内容居中");
        area->setAlignment(JZScrollArea::AlignRight | JZScrollArea::AlignBottom);
        check(content->x() == 200 - 60 && content->y() == 100 - 40, "对齐标志给右下 -> 靠右下");
        area->setAlignment(JZScrollArea::AlignLeft | JZScrollArea::AlignTop);

        // 内容比视口大时对齐标志不起作用 (位置由滚动偏移决定)
        content->resize(400, 200);
        area->setAlignment(JZScrollArea::AlignHCenter | JZScrollArea::AlignVCenter);
        check(content->x() == 0 && content->y() == 0, "内容溢出时不参与对齐 (仍贴左上)");

        check(area->resolvedStyle().background.isNone(), "默认不设底色 (仍是原来的浅灰/白)");
        check(area->setStyleSheet("JZScrollArea { background-color: #000000; }"),
              "滚动区样式表解析通过");
        const JZOptional<JZColor>& bg = area->resolvedStyle().background;
        check(bg.isSome() && bg.value().red() == 0 && bg.value().green() == 0 &&
              bg.value().blue() == 0,
              "样式表把滚动区底色设成黑 (viewport 同步, 见 eventFilter)");
    }

    // [temp] 复现宿主界面: JZMainWindow + ScrollArea + 屏幕视图 (与 virtual_device.exe 同构)
    {
        JZMainWindow host;
        host.resize(320, 260);
        JZScrollArea* area = new JZScrollArea();
        host.setCentralWidget(area);
        JZVirtualDevice desk2(800, 600);
        JZVirtualScreenView* view = new JZVirtualScreenView(&desk2);
        view->resize(desk2.desktopSize());
        area->setWidget(view);
        // 显式摆到桌面左上角: 宿主窗口比虚拟屏小 (滚动区), 只有这块区域在视野/帧缓冲里
        JZVirtualWindow* w2 = desk2.createWindow(200, 120, "host-win", 20, 20);
        check(w2 != nullptr, "宿主界面里也能正常建窗口");
        host.show();

        // ---- 与主机共享 DPI: 视图把宿主窗口的设备缩放同步给桌面 ----
        host.doPaint(host.rect());          // 同步走一遍绘制 (视图在 paintEvent 里同步 DPI)
        const float hostScale = host.device()->scale();
        check(desk2.scale() == hostScale, "视图把宿主 DPI 同步给桌面 (与主机共享缩放)");
        check(w2->client()->device()->scale() == hostScale, "客户区按同一缩放渲染");
        check(view->width() == 800 && view->height() == 600,
              "视图仍按逻辑尺寸布局 (桌面坐标与缩放无关)");
        check((float)w2->frame().width() == 200.0f * hostScale,
              "窗口帧缓冲 = 客户区逻辑尺寸 × 宿主缩放");

        // ---- 拖动时实时刷新: 未松开时宿主帧缓冲里窗口就已在新位置 ----
        // (常规 update() 只是排脏区、要等消息循环下一轮 —— 那时这里是旧位置)
        const int grabX = w2->frameX() + w2->frameWidth() - 60;   // 标题栏 (避开标题文字与按钮)
        const int grabY = w2->frameY() + 11;
        check(desk2.injectMouseAtScreen(JZMouseEvent::Press, JZMouseEvent::LeftButton,
                                        grabX, grabY), "按下标题栏");
        check(desk2.isDragging(), "拖动进行中");
        const int step = 60;
        desk2.injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton,
                                  grabX + step, grabY + step);
        JZImage frame;
        const bool haveFrame = host.device()->snapshot(frame) && !frame.isNull();
        check(haveFrame, "读到宿主窗口帧缓冲");
        if (haveFrame) {
            // 桌面坐标 -> 宿主客户区坐标 -> 帧缓冲里的物理像素
            auto toFramePx = [&](int sx, int sy) {
                const JZPoint p = view->mapTo(view->window(), JZPoint(sx, sy));
                return JZPoint((int)(p.x() * hostScale + 0.5f), (int)(p.y() * hostScale + 0.5f));
            };
            const JZPoint now = toFramePx(w2->frameX() + w2->frameWidth() - 60, w2->frameY() + 11);
            check(pixelIs(frame, now.x(), now.y(), 0x2b, 0x57, 0x9a),
                  "拖动中 (未松开) 标题栏已画在新位置 = 实时刷新, 不等下一轮消息循环");
            const JZPoint old = toFramePx(grabX, grabY);
            check(pixelIs(frame, old.x(), old.y(), 0, 128, 128),
                  "原位置已回到底色 (经典绿; 一帧就是重新合成的, 没有残影)");
        }
        // 拖动一步的代价 = 合成 (800x600 桌面 × 宿主缩放) + 宿主窗口重绘 —— 现在这一步是
        // **同步**做掉的 (见 JZVirtualDevice::refreshNow), 所以它就是"跟手延迟"的上限。
        // 只报数不设门限 (机器负载会变), 肉眼看它是否远小于一帧 16ms。
        {
            const int n = 20;
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i)
                desk2.injectMouseAtScreen(JZMouseEvent::Move, JZMouseEvent::LeftButton,
                                          grabX + step - i, grabY + step);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / n;
            printf("  [info] 拖动一步 (合成 + 重绘) 平均 %.2f ms\n", ms);
        }

        desk2.injectMouseAtScreen(JZMouseEvent::Release, JZMouseEvent::LeftButton,
                                  grabX + step, grabY + step);
        check(!desk2.isDragging(), "松开结束拖动");

        view->setScreen(nullptr);   // 先解绑: desk2 比宿主界面先析构 (视图不能留悬垂的 m_screen)
    }

    // ---- 存帧 (肉眼确认; 失败不算错) ----

    if (desk.screenshot().save("virtual_desktop_frame.png"))
        printf("  [info] 已保存整屏帧: virtual_desktop_frame.png\n");

    printf(g_fail == 0 ? "[虚拟桌面] 全部通过\n" : "[虚拟桌面] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
