// JZMenu::exec —— Qt QMenu::exec 语义的自测: **阻塞**弹出, 返回被触发的项。
//
// 覆盖:
//   ① 选中项 -> 返回那一项本身(指针相等), 且返回时弹层已收起
//   ② 子菜单里的项 -> 也能作为结果返回(信号/下标在子菜单身上, 结果沿父链写回)
//   ③ 取消(Esc/外点, 没有 activateAction) -> nullptr
//   ④ 空菜单 -> 立刻返回 nullptr(不弹也不阻塞)
//   ⑤ owner = nullptr -> 仍能弹(只是不做 DPI 换算 / 模态归属)
//
// ⚠ 这是"要看窗口"的自测(弹层是真的原生窗口 + 嵌套模态循环), 所以**不注册 ctest**
//    (与 test_widget / test_shooter 那些交互样例同一处理; 只有离屏也能跑的那几个才进 ctest)。
//    手动跑: build/test/widget/menu/Debug/test_menu_exec.exe

#include <cstdio>
#include <string>

#include "JZApplication.h"
#include "JZWindow.h"
#include "JZMenu.h"
#include "JZAction.h"
#include "JZTimer.h"

namespace
{

int g_failed = 0;

void check(bool ok, const std::string& what)
{
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", what.c_str());
    std::fflush(stdout);           // 自测要能在卡住时看到"卡在哪一步"
    if (!ok)
        ++g_failed;
}

void step(const char* what)
{
    std::printf("---- %s\n", what);
    std::fflush(stdout);
}

// "模拟用户点了第 n 项": 真点击走的就是 activateAction(先收起整条弹层链, 再触发),
// 这里用定时器在模态循环里调它 —— 定时器由框架的定时器队列驱动, 模态循环会泵它。
void clickLater(JZMenu* menu, int index, int ms)
{
    JZTimer::singleShot(ms, [menu, index]() {
        std::printf("     (tick: 模拟点击第 %d 项)\n", index);
        std::fflush(stdout);
        menu->activateAction(index);
    });
}

void runChecks(JZWindow* win)
{
    // ① 选中项
    {
        step("① 选中项");
        JZMenu menu;
        JZAction* a0 = menu.addAction("A");
        JZAction* a1 = menu.addAction("B");
        clickLater(&menu, 1, 260);

        JZAction* got = menu.exec(win, JZRect(60, 60, 1, 1));
        step("① exec 返回");
        check(got == a1, "exec() 返回被点的那一项(指针就是 addAction 返回的那个)");
        check(got != a0, "没被点的项不会返回");
        check(!menu.isPopupOpen(), "exec() 返回时弹层已经收起");
        check(a1->isEnabled(), "动作仍是可用状态(exec 不改动作状态)");
    }

    // ② 子菜单里的项
    {
        step("② 子菜单项");
        JZMenu menu;
        menu.addAction("A");
        JZMenu* sub = menu.addMenu("more");
        JZAction* subAct = sub->addAction("sub item");
        std::printf("     (父: actions=%d menus=%d | 子: actions=%d parent=%s)\n",
                    menu.actionCount(), menu.menuCount(), sub->actionCount(),
                    sub->parentMenu() == &menu ? "ok" : "NULL");
        std::fflush(stdout);
        clickLater(sub, 0, 260);          // 点在子菜单里
        // 看门狗: 正常 260ms 就该结束; 到点还没关说明没走通, 强制关掉好让后面的用例继续
        JZTimer::singleShot(1800, [&menu]() {
            if (menu.isPopupOpen()) {
                std::printf("     (watchdog: 弹层仍未关闭, 强制 hide)\n");
                std::fflush(stdout);
                menu.hidePopup();
            }
        });

        JZAction* got = menu.exec(win, JZRect(60, 60, 1, 1));
        step("② exec 返回");
        check(got == subAct, "子菜单里的项也能作为 exec() 的结果(沿父链写回发起者)");
    }

    // ③ 取消: 没有 activateAction, 只有关闭
    {
        JZMenu menu;
        menu.addAction("A");
        JZTimer::singleShot(260, [&menu]() { menu.hidePopup(); });

        JZAction* got = menu.exec(win, JZRect(60, 60, 1, 1));
        check(got == nullptr, "取消(Esc/点空白)时 exec() 返回 nullptr");
    }

    // ④ 空菜单: 不阻塞
    {
        JZMenu menu;
        JZAction* got = menu.exec(win, JZRect(60, 60, 1, 1));
        check(got == nullptr, "空菜单 exec() 立刻返回 nullptr");
    }

    // ⑤ owner = nullptr
    {
        JZMenu menu;
        JZAction* a = menu.addAction("A");
        clickLater(&menu, 0, 260);

        JZAction* got = menu.exec(nullptr, JZRect(200, 200, 1, 1));
        check(got == a, "owner=nullptr 也能 exec(弹层按 1.0 缩放定位)");
    }

    // ⑥ 嵌套: 某个项的处理器里又 exec 一个菜单 —— 内层结果不污染外层
    {
        JZMenu outer;
        JZMenu inner;
        JZAction* innerAct = inner.addAction("inner");

        // 外层第 0 项被触发 -> 处理器里再开内层, 并在内层选第 0 项
        JZAction* gotInnerResult = nullptr;
        JZAction* outerAct = outer.addAction("open inner", [&]() {
            clickLater(&inner, 0, 260);
            gotInnerResult = inner.exec(win, JZRect(120, 120, 1, 1));
        });
        clickLater(&outer, 0, 200);

        JZAction* outerResult = outer.exec(win, JZRect(60, 60, 1, 1));
        check(outerResult == outerAct, "外层 exec() 返回外层被点的项");
        check(gotInnerResult == innerAct, "内层 exec() 返回内层被点的项(嵌套各记各的)");
    }
}

int app_main(int argc, char* argv[])
{
    JZApplication app;

    JZWindow win;
    win.resize(320, 200);
    win.setTitle("JZMenu::exec self test");
    win.show();

    // 自测放进主循环的第一个 tick 里跑: exec() 内部是嵌套模态循环, 而"模拟点击"的定时器
    // 需要框架的定时器队列被泵 —— 主循环先起来与真实用法一致。
    JZTimer::singleShot(200, [&win, &app]() {
        step("主循环已起来, 开始自测");
        runChecks(&win);
        std::printf("\n==== %s (%d failed) ====\n", g_failed == 0 ? "ALL PASS" : "FAILED", g_failed);
        win.hide();
        app.exit(g_failed == 0 ? 0 : 1);
    });

    return app.exec();
}

}   // namespace

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 卡住时也要能看到进度
    return app_main(argc, argv);
}
