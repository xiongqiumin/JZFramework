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
#include <cmath>
#include <string>

#include "JZApplication.h"
#include "JZVirtualDevice.h"
#include "JZVirtualWindow.h"
#include "JZCheckbox.h"
#include "JZLineEdit.h"
#include "JZLabel.h"
#include "JZImage.h"
#include "JZImageLabel.h"
#include "JZComputerUse/JZComputerUse.h"
#include "json/json.h"
#include <cstdlib>

// OCR 兜底是否编入(由 CMake 在 HAS_JZCv 时定义; 见本目录 CMakeLists.txt)
#ifndef JZ_CU_OCR
#define JZ_CU_OCR 0
#endif
#if JZ_CU_OCR
#include <opencv2/imgproc.hpp>   // 合成"大字号文字图"给 OCR 读 (POWER)
#include <opencv2/core/utils/logger.hpp>
#endif

// 像素族(模板/颜色匹配)是否编入(由 jz_opencv_enable 定义; 见本目录 CMakeLists.txt)
#ifndef JZ_CU_OPENCV
#define JZ_CU_OPENCV 0
#endif
#if JZ_CU_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>   // 写模板 PNG(matchImage 的入参是文件路径)
#endif

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
#if JZ_CU_OCR
    // 关掉 OpenCV 的 INFO 噪音(它在找 TBB/OpenMP 并行插件, 找不到只是提示)
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
#endif
    JZApplication app;

    // ---- 被测应用: 一个"电源面板"窗口, 跑在虚拟桌面上 ----
    JZVirtualDevice desk(800, 600);
    desk.setDesktopBackground(JZColor(232, 232, 232, 255));

    JZVirtualWindow* panel = desk.createWindow(360, 220, "PowerPanel", 120, 90);
    // 控件挂到窗口的**客户区** (客户区那个真 JZWindow 才是控件树的根)
    JZCheckbox* power = new JZCheckbox(panel->client());
    power->setObjectName("btnPower");
    power->setText("Power");
    power->setGeometry(20, 20, 220, 40);
    power->show();

    JZLabel* stateLabel = new JZLabel(panel->client());
    stateLabel->setObjectName("lblState");
    stateLabel->setText("State: OFF");
    stateLabel->setGeometry(20, 80, 220, 30);
    stateLabel->show();

    JZLineEdit* note = new JZLineEdit(panel->client());
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
    // ⚠ 只查前缀是不够的: 真屏抓图的 dataURL 前缀一模一样。把图解回来, 它必须就是**整块虚拟桌面**
    //   —— 2026-09-30 就是靠这条(以及像素族用例)发现 matchImage/matchColor 当时抓的是本机真屏。
    {
        const JZImage back = JZScreenShot::decodeDataUrl(shot0);
        check(back.width() == desk.desktopWidth() && back.height() == desk.desktopHeight(),
              "screenshot() 解出来就是虚拟桌面那一帧 (不是抓真屏)");
    }

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
    JZLabel* frontLabel = new JZLabel(front->client());
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

#if JZ_CU_OCR
    printf("[链路] 退化兜底: OCR 按文字定位\n");
    // 合成一张大字号图贴进虚拟窗口 —— 不依赖框架字体(字号完全可控), OCR 面对的就是像素。
    // 场景: 目标**没有可访问布局**时, 只能靠像素里的文字定位(等价 Locator{kind:'ocr'})。
    {
        const char* envModel = std::getenv("JZ_CU_OCR_MODEL");
        const std::string modelRoot = envModel ? envModel
                                                : "C:/Users/7052/Desktop/work/lib/PaddleOCR/model";
        if (!JZComputerUse::initOcr(modelRoot)) {
            printf("  [skip] OCR 未就绪 (模型目录: %s) —— 这一节跳过\n", modelRoot.c_str());
        } else {
            check(JZComputerUse::ocrReady(), "OCR 就绪 (本地 PP-OCR)");

            JZVirtualWindow* ocrWin = desk.createWindow(360, 160, "OcrPanel", 420, 400);
            cv::Mat big(140, 340, CV_8UC3, cv::Scalar(255, 255, 255));
            cv::putText(big, "POWER", cv::Point(14, 96), cv::FONT_HERSHEY_SIMPLEX, 2.2,
                        cv::Scalar(0, 0, 0), 5, cv::LINE_AA);
            cv::Mat rgbaBig;
            cv::cvtColor(big, rgbaBig, cv::COLOR_BGR2RGBA);
            const JZImage bannerImg = JZImage::fromRGBA(rgbaBig.data, rgbaBig.cols, rgbaBig.rows);
            JZImageLabel* banner = new JZImageLabel(ocrWin->client());
            banner->setObjectName("imgBanner");
            banner->setGeometry(10, 10, 340, 140);
            banner->setImage(bannerImg);
            banner->show();
            ocrWin->renderNow();

            // ① 列出屏上文字
            const Json::Value texts = JZComputerUse::ocrTexts();
            bool sawPower = false;
            for (const auto& t : texts) {
                std::string s = t["text"].asString();
                for (auto& c : s) c = (char)tolower((unsigned char)c);
                if (s.find("power") != std::string::npos)
                    sawPower = true;
            }
            check(sawPower, "ocrTexts() 在虚拟桌面上读出 \"POWER\"");

            // ② 按文字定位 -> 归一化 rect (与 layoutTree/click 同口径)
            const CuRect hit = JZComputerUse::matchText("POWER");
            check(hit.w > 0 && hit.h > 0, "matchText(\"POWER\") 命中");

            const int hx = (int)(hit.x * desk.desktopWidth());
            const int hy = (int)(hit.y * desk.desktopHeight());
            check(desk.windowAt(hx, hy) == ocrWin,
                  "命中的归一化 rect 反算到桌面像素后落在 OCR 窗口上 (口径一致)");

            // ③ 用这个 rect 直接点 —— 退化路径同样能"定位 -> 操作"
            check(JZComputerUse::click(hit.x + hit.w * 0.5, hit.y + hit.h * 0.5),
                  "用 OCR 命中的 rect 点击成功");
            check(desk.activeWindow() == ocrWin, "点击后该窗口被激活");

            // ④ 找不到就是找不到(显式退化, 不假装成功)
            const CuRect miss = JZComputerUse::matchText("NoSuchTextXYZ");
            check(miss.w <= 0, "找不到的文字返回未命中 (不假装成功)");
        }
    }
#endif   // JZ_CU_OCR

#if JZ_CU_OPENCV
    printf("[链路] 退化兜底: 像素族(模板匹配 / 颜色)\n");
    // 目标**没有可访问布局**时只能靠像素定位。这一节验证两件事:
    //   ① 像素族在虚拟桌面上真能找着东西(不是"编译过但从没跑过");
    //   ② 它给的是**与布局/点击同一口径**的归一化矩形 —— 这是"建模期像素能认领到布局节点"的前提。
    {
        const int IW = 120, IH = 80;
        // 图案刻意做成"翻转不变"的对称标, 底色用**品红**(R==B): 对 R/B 互换免疫 ⇒
        // 这里只考形状与位置, 不把一个潜在的通道序问题混进坐标断言里。
        cv::Mat icon(IH, IW, CV_8UC3, cv::Scalar(255, 0, 255));                       // BGR 品红
        cv::rectangle(icon, cv::Rect(IW / 2 - 34, IH / 2 - 22, 68, 44), cv::Scalar(255, 255, 255), cv::FILLED);
        cv::line(icon, cv::Point(IW / 2, IH / 2 - 22), cv::Point(IW / 2, IH / 2 + 22), cv::Scalar(0, 0, 0), 6);
        cv::line(icon, cv::Point(IW / 2 - 34, IH / 2), cv::Point(IW / 2 + 34, IH / 2), cv::Scalar(0, 0, 0), 6);

        // matchImage 的入参是**文件路径** ⇒ 这才是它的真实调用形态(模板由建模期/人工准备)
        const std::string tplPath = "cu_template_icon.png";
        check(cv::imwrite(tplPath, icon), "模板文件已写出 (matchImage 的入参是文件路径)");

        JZVirtualWindow* pixelWin = desk.createWindow(200, 120, "PixelPanel", 430, 30);
        cv::Mat rgbaIcon;
        cv::cvtColor(icon, rgbaIcon, cv::COLOR_BGR2RGBA);
        JZImageLabel* iconLabel = new JZImageLabel(pixelWin->client());
        iconLabel->setObjectName("imgIcon");
        iconLabel->setGeometry(40, 20, IW, IH);
        iconLabel->setImage(JZImage::fromRGBA(rgbaIcon.data, rgbaIcon.cols, rgbaIcon.rows));
        iconLabel->show();
        pixelWin->renderNow();
        desk.activate(pixelWin);        // 置顶: 保证这块图案在合成帧里不被别的窗口压住

        // 期望位置 = 控件在**虚拟桌面**上的坐标。客户端原点与窗口原点重合(见
        // JZVirtualDevice::clientToScreen), 所以 = 窗口桌面坐标 + 控件客户区坐标;
        // layoutTree 也是这么把客户区坐标搬上桌面的(见 shiftBoundsInScreen)。
        // ⚠ 不用 mapToGlobal: 虚拟窗口没有平台窗口, 它的"global"不是桌面坐标 —— 坐标系归桌面所有。
        const JZPoint origin = desk.clientToScreen(pixelWin, 40, 20);
        const JZPoint viaMap = iconLabel->mapToGlobal(JZPoint(0, 0));
        printf("  [info] 期望原点: clientToScreen=(%d,%d) mapToGlobal=(%d,%d)\n",
               origin.x(), origin.y(), viaMap.x(), viaMap.y());
        const double ex = (origin.x() + IW * 0.5) / desk.desktopWidth();
        const double ey = (origin.y() + IH * 0.5) / desk.desktopHeight();

        auto rectAt = [](double x, double y, double w, double h) { CuRect r; r.x = x; r.y = y; r.w = w; r.h = h; return r; };
        const double tolPx = 2.0;

        // ① 模板匹配: 全屏找
        const CuRect hit = JZComputerUse::matchImage(tplPath, CuRect());
        printf("  [info] matchImage -> (%.4f, %.4f, %.4f, %.4f)\n", hit.x, hit.y, hit.w, hit.h);
        check(hit.w > 0 && hit.h > 0, "matchImage(模板文件) 命中");
        check(std::fabs((hit.x + hit.w * 0.5) - ex) * desk.desktopWidth() <= tolPx &&
              std::fabs((hit.y + hit.h * 0.5) - ey) * desk.desktopHeight() <= tolPx,
              "命中中心 == 控件桌面坐标 (像素族与布局/点击同口径)");
        check(std::fabs(hit.w * desk.desktopWidth() - IW) <= tolPx &&
              std::fabs(hit.h * desk.desktopHeight() - IH) <= tolPx,
              "命中尺寸 == 模板尺寸 (单尺度匹配)");

        // ② region 限定: 目标在左上 1/4 之外 ⇒ 该区域里必须找不到(region 换算真生效)
        // ⚠ region 是"搜索窗口", 模板必须**整个**落在里面 (matchTemplate 的语义) ——
        //   窗口有外框之后图案整体下移了一格标题栏, 所以这里的 region 要够高。
        check(JZComputerUse::matchImage(tplPath, rectAt(0.0, 0.0, 0.25, 0.25)).w <= 0,
              "matchImage 受 region 限制: 区域外找不到");
        check(JZComputerUse::matchImage(tplPath, rectAt(0.5, 0.0, 0.25, 0.4)).w > 0,
              "matchImage 在给定 region 内仍命中");

        // ③ 颜色查找: 品红在全屏唯一
        const CuRect col = JZComputerUse::matchColor("#FF00FF", CuRect());
        printf("  [info] matchColor  -> (%.4f, %.4f, %.4f, %.4f)\n", col.x, col.y, col.w, col.h);
        check(col.w > 0 && col.h > 0, "matchColor 命中唯一的颜色块");
        check(std::fabs((col.x + col.w * 0.5) - ex) * desk.desktopWidth() <= tolPx &&
              std::fabs((col.y + col.h * 0.5) - ey) * desk.desktopHeight() <= tolPx,
              "颜色命中中心 == 控件桌面坐标");

        // ④ 用像素族命中的矩形直接点 —— 退化路径同样能"定位 -> 操作"
        check(JZComputerUse::click(col.x + col.w * 0.5, col.y + col.h * 0.5),
              "用 matchColor 的命中矩形点击成功");
        check(desk.activeWindow() == pixelWin, "点击后该窗口被激活");

        // ⑤ 找不到就是找不到(显式退化, 不假装成功)
        check(JZComputerUse::matchColor("#00FF00", CuRect()).w <= 0,
              "找不到的颜色返回未命中 (不假装成功)");
    }
#endif   // JZ_CU_OPENCV

    printf("[链路] 收尾\n");
    // ⑪ 可以切回真实桌面
    JZComputerUse::setVirtualDesktop(nullptr);
    check(!JZComputerUse::isVirtualDesktop(), "setVirtualDesktop(nullptr) 切回真实桌面");

    if (desk.screenshot().save("cu_virtual_frame.png"))
        printf("  [info] 已保存整屏帧: cu_virtual_frame.png\n");

    printf(g_fail == 0 ? "[链路] 全部通过\n" : "[链路] 失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
