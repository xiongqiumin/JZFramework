// ==================== 整条链路: computer-use -> 虚拟桌面 -> 应用 ====================
// 这里调的 **就是一份 CU 脚本会走的那批底层 API**(JZComputerUse 的静态接口 = JS 侧
// `__jz.computeruse.*` 的底层实现), 只是用 C++ 调:
//
//   截图 -> layoutTree 找控件 -> 用**归一化坐标** click -> 读状态 -> 再截图
//
// 链路形状:
//   JZComputerUse            (归一化坐标 / 语义树 / 稳定判定 —— 脚本看到的那层)
//     └─ JZVirtualDevice     (桌面坐标系 + 命中窗口 + 注入; 由 setVirtualDesktop 挂上)
//          └─ JZVirtualWindow (无原生窗口的顶层窗口, 有桌面坐标与 z 序)
//               └─ 控件        (JZCheckbox / JZLineEdit / JZLabel)
//
// 验证的重点:
//   ① 布局树就是"给脚本看的可访问树"(source=semantic, rect 归一化到整块桌面);
//   ② 归一化 rect 与 click 反算到**同一个桌面像素** —— 这是"能定位就能点中"的根;
//   ③ 读状态走布局 (checked/text), 不碰像素;
//   ④ 命中按 z 序, focusWindow 能切目标;
//   ⑤ 虚拟模式下"启动进程/杀进程/切前台"一律明确失败 —— 绝不误动真实系统。
//
// 本测试不建任何真实窗口 => 可无头跑, 已进 ctest。
#include <cstdio>
#include <cstring>
#include <string>

#include "JZApplication.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZCheckbox.h"
#include "JZLineEdit.h"
#include "JZLabel.h"
#include "JZImage.h"
#include "JZComputerUse/JZComputerUse.h"
#include "json/json.h"

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

// 在布局树里按 id(= viewId/objectName) 找节点
static bool findById(const Json::Value& n, const std::string& id, Json::Value& out)
{
    if (!n.isObject())
        return false;
    if (n.isMember("id") && n["id"].asString() == id) {
        out = n;
        return true;
    }
    const Json::Value& kids = n["children"];
    if (kids.isArray()) {
        for (const auto& c : kids) {
            if (findById(c, id, out))
                return true;
        }
    }
    return false;
}

// 节点归一化 rect 的中心 (click 用它)
static bool rectCenter(const Json::Value& node, double& nx, double& ny)
{
    const Json::Value& r = node["rect"];
    if (!r.isObject() || r["w"].asDouble() <= 0)
        return false;
    nx = r["x"].asDouble() + r["w"].asDouble() * 0.5;
    ny = r["y"].asDouble() + r["h"].asDouble() * 0.5;
    return true;
}

// 找控件 + 给出它的归一化中心 (脚本里就是这两步: 定位 -> 点击)
static bool locate(const std::string& title, const std::string& id, double& nx, double& ny)
{
    Json::Value env = JZComputerUse::layoutTree(title);
    Json::Value node;
    if (!findById(env["tree"], id, node))
        return false;
    return rectCenter(node, nx, ny);
}

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲: 崩了也能看到走到哪一步
    JZApplication app;

    // ---- 被测应用: 一个"电源面板"窗口, 跑在虚拟桌面上 ----
    JZVirtualDevice desk(800, 600, "Virtual Desktop");
    desk.setDesktopBackground(JZColor(232, 232, 232, 255));

    JZVirtualWindow* panel = desk.createWindow(360, 220, "PowerPanel", 120, 90);
    JZCheckbox* power = new JZCheckbox(panel);
    power->setObjectName("btnPower");
    power->setText("Power");
    power->setGeometry(20, 20, 220, 40);
    power->show();

    JZLabel* stateLabel = new JZLabel(panel);
    stateLabel->setObjectName("lblState");
    stateLabel->setText("State: OFF");
    stateLabel->setGeometry(20, 80, 220, 30);
    stateLabel->show();

    JZLineEdit* note = new JZLineEdit(panel);
    note->setObjectName("editNote");
    note->setGeometry(20, 130, 220, 32);
    note->show();

    panel->renderNow();

    // ---- 把这块虚拟桌面接到 CU 上 (这就是 computeUse->setBackend(...) 那一步) ----
    JZComputerUse::setVirtualDesktop(&desk);
    check(JZComputerUse::isVirtualDesktop(), "CU 后端已切到虚拟桌面");
    check(JZComputerUse::virtualDesktop() == &desk, "后端就是这块桌面");

    printf("[链路] 截图 / 布局树\n");

    // ① 截图: 走桌面合成帧, 不抓屏
    const std::string shot0 = JZComputerUse::screenshot();
    check(shot0.rfind("data:image/png;base64,", 0) == 0, "screenshot() 返回 PNG dataURL");

    // ② 布局树: 按标题找窗口 -> 语义树
    Json::Value env = JZComputerUse::layoutTree("PowerPanel");
    check(env["source"].asString() == "semantic", "layoutTree 走语义树 (source=semantic)");

    Json::Value btn;
    check(findById(env["tree"], "btnPower", btn), "布局树里按 id 找到按钮 (viewId 稳定)");
    check(btn.isMember("checked") && btn["checked"].asBool() == false, "布局树里能读到状态 (checked=false)");
    check(btn["rect"].isObject() && btn["rect"]["w"].asDouble() > 0, "节点带归一化 rect");

    // 归一化 rect 属于"整块桌面"口径: 反算回桌面像素应落在窗口内
    {
        const double nx = btn["rect"]["x"].asDouble();
        const double ny = btn["rect"]["y"].asDouble();
        const int dx = (int)(nx * desk.desktopWidth());
        const int dy = (int)(ny * desk.desktopHeight());
        check(desk.windowAt(dx, dy) == panel, "归一化 rect 反算到桌面像素后, 命中目标窗口");
    }

    printf("[链路] 点击 / 读状态\n");

    // ③ 点击: 归一化中心 -> 桌面像素 -> 命中窗口 -> 投递控件
    double nx = 0, ny = 0;
    check(locate("PowerPanel", "btnPower", nx, ny), "定位到按钮中心 (布局树)");
    check(!power->isChecked(), "点击前未勾选");
    check(JZComputerUse::click(nx, ny), "click(归一化坐标) 成功");
    check(power->isChecked(), "点击后控件状态变化 (能定位就能点中)");

    // ④ 读状态: 再取一次布局树 (走布局, 不碰像素)
    {
        Json::Value env2 = JZComputerUse::layoutTree("PowerPanel");
        Json::Value btn2;
        check(findById(env2["tree"], "btnPower", btn2) && btn2["checked"].asBool(),
              "读状态: 布局树里 checked=true");
    }

    // ⑤ 帧跟着状态变
    {
        const std::string shot1 = JZComputerUse::screenshot();
        check(shot0 != shot1, "CU 截图跟着界面状态变化");
    }

    printf("[链路] 键盘 / 稳定 / 切窗口\n");

    // ⑥ 键盘: 点输入框拿焦点, 再输入文本
    {
        double ex = 0, ey = 0;
        check(locate("PowerPanel", "editNote", ex, ey), "定位到输入框中心");
        JZComputerUse::click(ex, ey);
        JZComputerUse::type("hi");
        check(note->text() == "hi", "type() 落到焦点控件 (lineEdit == hi)");
    }

    // ⑦ 稳定判定: 虚拟桌面下恒真 (注入是同步的)
    check(JZComputerUse::waitStable(100, 500), "waitStable() 恒真 (同步渲染, 不用等)");

    // ⑧ 切窗口
    check(JZComputerUse::focusWindow("PowerPanel"), "focusWindow 命中已存在的窗口");
    check(!JZComputerUse::focusWindow("NoSuchWindow"), "focusWindow 找不到时返回 false");

    // ⑨ 虚拟模式下**不适用**的能力必须明确失败 (绝不误动真实系统)
    check(JZComputerUse::startProcess("cmd.exe") == 0, "startProcess 在虚拟桌面下不启动真实进程");
    check(!JZComputerUse::terminateProcess("explorer"), "terminateProcess 不去杀真实进程");
    check(!JZComputerUse::focusProcess("explorer"), "focusProcess 无进程语义");
    check(!JZComputerUse::launch("cmd.exe", "", "PowerPanel", 100), "launch 不启动真实程序");

    printf("[链路] 多窗口: 命中按 z 序\n");

    // ⑩ 新窗口压住面板: 同一个点应该打在**上层**窗口上, 面板状态不变
    JZVirtualWindow* front = desk.createWindow(360, 220, "SecondWin", 150, 130);
    JZLabel* frontLabel = new JZLabel(front);
    frontLabel->setObjectName("lblFront");
    frontLabel->setText("front");
    frontLabel->setGeometry(10, 10, 200, 30);
    frontLabel->show();
    front->renderNow();

    power->setChecked(false);
    double bx = 0, by = 0;
    check(locate("PowerPanel", "btnPower", bx, by), "上层有窗口时仍能按标题定位到面板");
    JZComputerUse::click(bx, by);
    check(!power->isChecked(), "该点被上层窗口截住 (命中按 z 序, 与真桌面一致)");

    check(JZComputerUse::focusWindow("PowerPanel"), "focusWindow 把面板置顶");
    JZComputerUse::click(bx, by);
    check(power->isChecked(), "置顶后再点命中面板 (focusWindow 真的改变了 z 序)");

    printf("[链路] 收尾\n");

    // ⑪ 可以切回真实桌面
    JZComputerUse::setVirtualDesktop(nullptr);
    check(!JZComputerUse::isVirtualDesktop(), "setVirtualDesktop(nullptr) 切回真实桌面");

    if (desk.screenshot().save("cu_virtual_frame.png"))
        printf("  [info] 已保存整屏帧: cu_virtual_frame.png\n");

    printf(g_fail == 0 ? "[链路] 全部通过\n" : "[链路] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
