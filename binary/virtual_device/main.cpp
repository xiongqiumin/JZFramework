// ==================== 虚拟桌面宿主 (VirtualVm) ====================
// 一台"虚拟机"的样子: JZMainWindow + 顶部菜单 + 中间滚动区, 滚动区里就是那块**虚拟屏幕**
// (默认显示器 800 x 600)。屏幕上跑着若干虚拟窗口:
//   · 窗口的**客户区**就是一个真 JZWindow (控件/布局/事件/渲染走框架同一条链);
//   · 外框 (标题栏 + 边框 + 最小化/最大化/关闭) 由虚拟桌面的窗口管理器画与命中;
//   · 用户在宿主窗口里点/拖 = 换算成桌面坐标后投给对应窗口 (见 JZVirtualScreenView);
//   · 屏幕底部是**任务栏 + 开始菜单** (桌面 chrome): 每个窗口一个按钮, 最大化只铺到
//     工作区 (不盖任务栏); 菜单项由这里登记 (见 addStartMenuItem)。
//   · **HTTP 接口** (见 JZVirtualDeviceHttp): 从外面加载程序/截图/收鼠标键盘 ——
//     加 `--headless --http-port N` 就是一台**容器**: 不显示界面, 谁都能驱动它。
//
// 这块桌面**不依赖宿主界面**: 不 show 宿主也能跑 (无头/回放), 宿主只是"显示器"。
#include "JZApplication.h"
#include "JZMainWindow.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZScrollArea.h"
#include "JZVirtualDevice.h"
#include "JZVirtualDeviceHttp.h"   // HTTP 接口 (容器模式)
#include "JZVirtualScreenView.h"
#include "JZVirtualWindow.h"
#include "JZVirtualProcess.h"   // 一个进程 = 一个线程 + 一份 JS 运行时
#include "JZWindow.h"
#include "JZLabel.h"
#include "JZPushButton.h"
#include "JZLineEdit.h"
#include "JZCheckbox.h"
#include "JZSignal.h"
#include "JZDebug.h"   // jzWarning
#include "JZComputerUse/JZComputerUse.h"   // 把 CU 后端指向本容器这块桌面 (见构造函数)

#include <string>
#include <cstdio>
#include <cstdlib>

namespace {

const int kDesktopW = 800;      // 虚拟显示器默认尺寸
const int kDesktopH = 600;

const int kDefaultHttpPort = 8720;   // HTTP 接口默认端口 (0 = 关掉)

// 命令行 (容器用法):
//   virtual_device.exe                                  交互: 有界面, 带两个示范窗口
//   virtual_device.exe --headless --http-port 8720      容器: 无界面 + 干净桌面, 由 HTTP 驱动
//   virtual_device.exe --program app.js                 直接加载一个 JS 程序 (也可以之后用 HTTP 加载)
struct Options
{
    int desktopW = kDesktopW;
    int desktopH = kDesktopH;
    int httpPort = kDefaultHttpPort;
    bool headless = false;      // 容器模式: 不显示宿主界面 + 不预建示范窗口
    std::string program;        // --program: 开机就加载的 JS 程序 (文件路径)
    std::string programName;    // --program-name: 它的程序名 (缺省 = 文件名去扩展名)
};

void printUsage()
{
    printf("用法: virtual_device.exe [选项]\n"
           "  --http-port <n>     HTTP 接口端口 (0 = 关掉; 默认 %d)\n"
           "  --headless          容器模式: 不显示宿主界面, 桌面保持干净 (无示范窗口)\n"
           "  --desktop <WxH>     虚拟显示器尺寸 (默认 %dx%d)\n"
           "  --program <file>    开机加载一个 JS 程序 (按 ESM 模块加载, 运行根 = 它所在目录)\n"
           "  --program-name <名> 上面那个程序的程序名 (缺省 = 文件名去扩展名)\n"
           "  例: virtual_device.exe --headless --http-port 8720 --program app.js\n"
           "      virtual_device.exe --headless --program scenes/gen.js --program-name scene\n",
           kDefaultHttpPort, kDesktopW, kDesktopH);
}

// 返回 0 = 继续, 1 = 已经处理完 (帮助/参数错, 调用方直接退出)
int parseArgs(int argc, char* argv[], Options& opt)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--headless") {
            opt.headless = true;
        } else if (a == "--http-port" && i + 1 < argc) {
            opt.httpPort = atoi(argv[++i]);
        } else if (a == "--program" && i + 1 < argc) {
            opt.program = argv[++i];
        } else if (a == "--program-name" && i + 1 < argc) {
            opt.programName = argv[++i];
        } else if (a == "--desktop" && i + 1 < argc) {
            int w = 0, h = 0;
            if (sscanf(argv[++i], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
                printf("--desktop 要写成 WxH (例 800x600)\n");
                return 1;
            }
            opt.desktopW = w;
            opt.desktopH = h;
        } else if (a == "-h" || a == "--help") {
            printUsage();
            return 1;
        } else {
            printf("不认识的参数: %s\n", a.c_str());
            printUsage();
            return 1;
        }
    }
    return 0;
}

// "app.js" -> "app" (缺省程序名; 只影响桌面上的显示与 /api/programs 的键),
// 同时给出它所在目录 (程序的运行根: 引擎按它找模块/相对路径)
void splitPath(const std::string& path, std::string& dir, std::string& base)
{
    const int slash = (int)path.find_last_of("/\\");
    dir = (slash > 0) ? path.substr(0, (size_t)slash) : std::string();
    base = (slash >= 0) ? path.substr((size_t)slash + 1) : path;
}

std::string programNameOf(const std::string& path)
{
    std::string dir, base;
    splitPath(path, dir, base);
    const int dot = (int)base.find_last_of('.');
    if (dot > 0)
        base = base.substr(0, (size_t)dot);
    return base.empty() ? std::string("app") : base;
}

} // namespace

class VirtualDeviceApp : public JZMainWindow
{
public:
    explicit VirtualDeviceApp(const Options& opt)
        : m_opt(opt), m_screen(opt.desktopW, opt.desktopH)
    {
        setTitle("虚拟桌面");
        resize(1000, 720);

        // ---- 菜单栏 ----
        JZMenuBar* bar = new JZMenuBar();
        addMenuBar(bar);

        JZMenu* fileMenu = bar->addMenu("文件");
        fileMenu->addAction("新建窗口", [this]() { newDemoWindow(); });
        fileMenu->addAction("关闭当前窗口", [this]() { closeActiveWindow(); });
        fileMenu->addSeparator();
        fileMenu->addAction("退出", [this]() { close(); });

        JZMenu* winMenu = bar->addMenu("窗口");
        winMenu->addAction("层叠", [this]() { cascade(); });
        winMenu->addAction("活动窗口最大化", [this]() {
            if (JZVirtualWindow* w = m_screen.activeWindow())
                w->setMaximized(!w->isMaximized());
        });
        winMenu->addAction("显示桌面", [this]() { m_screen.refresh(); });

        JZMenu* procMenu = bar->addMenu("进程");
        procMenu->addAction("新建 JS 进程窗口", [this]() { newJsProcessWindow(); });
        procMenu->addAction("停掉所有进程", [this]() { m_screen.shutdownProcesses(); });

        JZMenu* viewMenu = bar->addMenu("视图");
        viewMenu->addAction("经典绿桌面", [this]() {
            m_screen.setDesktopBackground(JZColor(0, 128, 128, 255));   // 默认
        });
        viewMenu->addAction("白色桌面", [this]() {
            m_screen.setDesktopBackground(JZColor(255, 255, 255, 255));
        });
        viewMenu->addAction("浅灰桌面", [this]() {
            m_screen.setDesktopBackground(JZColor(232, 232, 232, 255));
        });

        // ---- 开始菜单 (桌面任务栏左边那个按钮; 菜单项就是这些动作) ----
        // 桌面只管画与命中, 动作由宿主登记 —— 于是"新建窗口/起 JS 进程"这类宿主能力
        // 不需要被桌面认识。
        m_screen.addStartMenuItem("新建窗口", [this]() { newDemoWindow(); });
        m_screen.addStartMenuItem("新建 JS 进程窗口", [this]() { newJsProcessWindow(); });
        m_screen.addStartMenuItem("关闭当前窗口", [this]() { closeActiveWindow(); });
        m_screen.addStartMenuItem("窗口层叠", [this]() { cascade(); });
        m_screen.addStartMenuItem("停掉所有进程", [this]() { m_screen.shutdownProcesses(); });
        m_screen.addStartMenuItem("关机", [this]() {
            m_screen.shutdownProcesses();   // 先收掉跑着的进程 (线程里的 JS 收干净再关)
            close();                        // = "关掉这台虚拟机" (宿主窗口关闭 -> 应用退出)
        });

        // ---- 中间: 滚动区 + 那块"屏幕" ----
        // 屏幕是 800x600, 宿主窗口更小的时候滚动区自己出滚动条 (真实分辨率语义);
        // 视口比屏幕大时把屏幕**居中**摆 (Qt QScrollArea::alignment 语义), 余下区域刷黑。
        m_area = new JZScrollArea();
        m_area->setStyleSheet("JZScrollArea { background-color: #000000; }");   // 默认不黑, 这里要黑
        m_area->setAlignment(JZScrollArea::AlignHCenter | JZScrollArea::AlignVCenter);
        setCentralWidget(m_area);

        m_view = new JZVirtualScreenView(&m_screen);   // 显示控件: 画合成图 + 转发输入
        m_view->resize(m_screen.desktopSize());
        m_area->setWidget(m_view);                     // 归滚动区所有 (成为 viewport 子控件)

        // ---- 把 computer use 的后端指向**本容器这块桌面** ----
        // 于是容器里跑的 JS 程序可以直接用 __jz.computeruse.* (screenshot/click/key/wait...)
        // 操作同一块屏幕上的别的窗口 —— 脚本就按"像人一样操作这台小电脑"的方式写
        // (例: auto_code/game_play/play/play.ts)。宿主侧同样受益: 宿主里调的 CU 也就是这块桌面。
        // ⚠ 必须赶在下面的 --program / HTTP 加载程序之前接上: 程序一起来就可能调 CU API。
        JZComputerUse::setVirtualDesktop(&m_screen);

        // ---- HTTP 接口 (把这块桌面当容器驱动: 加载程序 / 截图 / 鼠标 / 键盘) ----
        // 宿主这边只要两行: 建接口 + 把"程序管理"接到虚拟进程上 (见 JZVirtualDeviceHttp.h)。
        if (m_opt.httpPort > 0) {
            m_api = new JZVirtualDeviceHttp(&m_screen, this);
            jzAttachVirtualProgramHost(&m_screen, m_api);
            if (!m_api->listen(m_opt.httpPort))
                jzWarning() << "[api] 端口 " << m_opt.httpPort << " 起不来 (被占用了?)";
            else
                printf("[api] 容器就绪: http://127.0.0.1:%d\n", m_api->port());
        }

        if (!m_opt.headless) {      // 容器模式: 桌面保持干净 (窗口由程序/接口带来)
            newDemoWindow();
            newDemoWindow();
        }

        // ---- 开机加载的程序 (--program): 与 HTTP 的 POST /api/programs 是同一条路 ----
        // 按 ESM 模块加载, 运行根 = 程序所在目录 (于是 import 与 jzApp.applicationDirPath() 都对)。
        if (!m_opt.program.empty()) {
            const std::string name = m_opt.programName.empty() ? programNameOf(m_opt.program)
                                                               : m_opt.programName;
            std::string dir, base;
            splitPath(m_opt.program, dir, base);
            JZVirtualProcess* p = m_screen.createProcess(name, std::string(), dir, base);
            if (!p) {
                jzWarning() << "[program] 起进程失败: " << m_opt.program;
            } else if (!p->waitWindow(8000)) {
                jzWarning() << "[program] " << name << " 起来了但没等到窗口 "
                            << (p->lastError().empty() ? "(纯脚本?)" : p->lastError());
            } else {
                printf("[program] %s 已加载, 窗口: %s\n", name.c_str(),
                       p->window() ? p->window()->title().c_str() : "");
            }
        }
    }

    // ---- 一个"应用窗口"的示范: 客户区里放几个控件, 证明键盘/鼠标真落到窗口里 ----
    void newDemoWindow()
    {
        static int seq = 0;
        ++seq;

        JZVirtualWindow* w = m_screen.createWindow(400, 220, "窗口 " + std::to_string(seq));
        JZWindow* client = w->client();      // 客户区: 普通 JZWindow, 控件直接挂它下面

        JZLabel* tip = new JZLabel("客户区 = JZWindow (控件/事件/渲染与真窗口一致)", client);
        tip->setGeometry(12, 10, 370, 24);
        tip->show();

        JZLineEdit* edit = new JZLineEdit(client);
        edit->setGeometry(12, 44, 370, 28);
        edit->show();

        JZCheckbox* box = new JZCheckbox(client);
        box->setText("启用");
        box->setGeometry(12, 84, 120, 24);
        box->show();

        JZLabel* echo = new JZLabel("输入框内容: ", client);
        echo->setGeometry(12, 118, 370, 24);
        echo->show();

        JZPushButton* btn = new JZPushButton("读取输入框", client);
        btn->setGeometry(12, 150, 140, 30);
        btn->show();
        connect(&btn->sigClicked, this, [edit, echo]() {
            echo->setText("输入框内容: " + edit->text());
        });

        w->renderNow();                      // 出首帧 (合成时读的就是它)
    }

    // ---- 一个"虚拟机进程": 一个线程 + 一份 JS 运行时(JZEngineRuntime/JZJSEngine) ----
    // 它里面跑的 JS 自己建窗口 —— 窗口就在那个线程上, 与别的进程互不干扰。
    void newJsProcessWindow()
    {
        static int seq = 0;
        ++seq;
        const std::string name = "app" + std::to_string(seq);
        const std::string script =
            "const w = new JZWindow(380, 220, 'JS 进程窗口 " + std::to_string(seq) + "');"
            "const tip = new JZLabel('这个窗口跑在它自己的线程 + 独立 JS 引擎里', w);"
            "tip.setGeometry(12, 8, 350, 24); tip.show();"
            "const e = new JZLineEdit(w); e.setGeometry(12, 40, 350, 28); e.show();"
            "const out = new JZLabel('输入: ', w); out.setGeometry(12, 74, 350, 24); out.show();"
            "const b = new JZPushButton('读取输入框');"
            "b.setParentWidget(w); b.setGeometry(12, 110, 150, 30); b.show();"
            "b.onClicked = () => { out.setText('输入: ' + e.text()); };"
            "w.show();";

        JZVirtualProcess* p = m_screen.createProcess(name, script);
        if (p && !p->waitWindow(5000))
            jzWarning() << "[vproc] " << name << " 没能建出窗口: " << p->lastError();
        m_screen.refresh();
    }

    void closeActiveWindow()
    {
        // 与点标题栏的 X 同一条路: 发 close 消息 (应用可 ignore; 接受才从桌面摘下来)
        if (JZVirtualWindow* w = m_screen.activeWindow())
            w->close();
    }

    void cascade()
    {
        const std::vector<JZVirtualWindow*>& list = m_screen.windows();
        for (size_t i = 0; i < list.size(); ++i)
            list[i]->setFramePos(20 + (int)i * 28, 20 + (int)i * 28);
        m_screen.refresh();
    }

private:
    Options m_opt;                       // 命令行 (必须在 m_screen 之前: 桌面尺寸从它来)
    JZVirtualDevice m_screen;            // 虚拟显示器 + 窗口管理器
    JZScrollArea* m_area = nullptr;      // 中央滚动区
    JZVirtualScreenView* m_view = nullptr;   // 滚动区里的"屏幕"控件 (归滚动区所有)
    JZVirtualDeviceHttp* m_api = nullptr;    // HTTP 接口 (容器模式; 归本窗口所有)
};

int main(int argc, char* argv[])
{
    // 被当容器用时 stdout 常被 driver 重定向到文件: 关掉缓冲, 日志/报错立刻落盘
    // (崩了也知道走到哪一步; 这是容器该有的样子)
    setvbuf(stdout, nullptr, _IONBF, 0);

    Options opt;
    if (parseArgs(argc, argv, opt) != 0)
        return 0;

    JZApplication app;
    if (opt.headless)
    {
        // 容器: "窗口都没了" 不是退出条件 —— 无头下**连一个顶层窗口都没有**, 于是关掉任何
        // 一个虚拟窗口都会触发 notifyWindowClosed -> quit (实测: 停掉一个程序就把容器关了)。
        // 容器的生命周期由外面决定 (关掉进程), 这里不自己退。
        app.setQuitOnLastWindowClosed(false);
    }

    VirtualDeviceApp w(opt);
    if (!opt.headless)
        w.show();

    return app.exec();
}
