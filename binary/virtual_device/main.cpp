// ==================== 虚拟桌面宿主 (VirtualVm) ====================
// 一台"虚拟机"的样子: JZMainWindow + 顶部菜单 + 中间滚动区, 滚动区里就是那块**虚拟屏幕**
// (默认显示器 800 x 600)。屏幕上跑着若干虚拟窗口:
//   · 窗口的**客户区**就是一个真 JZWindow (控件/布局/事件/渲染走框架同一条链);
//   · 外框 (标题栏 + 边框 + 最小化/最大化/关闭) 由虚拟桌面的窗口管理器画与命中;
//   · 用户在宿主窗口里点/拖 = 换算成桌面坐标后投给对应窗口 (见 JZVirtualScreenView)。
//
// 这块桌面**不依赖宿主界面**: 不 show 宿主也能跑 (无头/回放), 宿主只是"显示器"。
#include "JZApplication.h"
#include "JZMainWindow.h"
#include "JZMenuBar.h"
#include "JZMenu.h"
#include "JZScrollArea.h"
#include "JZVirtualDevice.h"
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

#include <string>
#include <cstdio>

namespace {

const int kDesktopW = 800;      // 虚拟显示器默认尺寸
const int kDesktopH = 600;

} // namespace

class VirtualDeviceApp : public JZMainWindow
{
public:
    VirtualDeviceApp()
        : m_screen(kDesktopW, kDesktopH)
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
        viewMenu->addAction("白色桌面", [this]() {
            m_screen.setDesktopBackground(JZColor(255, 255, 255, 255));
        });
        viewMenu->addAction("浅灰桌面", [this]() {
            m_screen.setDesktopBackground(JZColor(232, 232, 232, 255));
        });

        // ---- 中间: 滚动区 + 那块"屏幕" ----
        // 屏幕是 800x600, 宿主窗口更小的时候滚动区自己出滚动条 (真实分辨率语义)。
        m_area = new JZScrollArea();
        setCentralWidget(m_area);

        m_view = new JZVirtualScreenView(&m_screen);   // 显示控件: 画合成图 + 转发输入
        m_view->resize(m_screen.desktopSize());
        m_area->setWidget(m_view);                     // 归滚动区所有 (成为 viewport 子控件)

        newDemoWindow();
        newDemoWindow();
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
        if (JZVirtualWindow* w = m_screen.activeWindow())
            m_screen.destroyWindow(w);
    }

    void cascade()
    {
        const std::vector<JZVirtualWindow*>& list = m_screen.windows();
        for (size_t i = 0; i < list.size(); ++i)
            list[i]->setFramePos(20 + (int)i * 28, 20 + (int)i * 28);
        m_screen.refresh();
    }

private:
    JZVirtualDevice m_screen;            // 虚拟显示器 + 窗口管理器
    JZScrollArea* m_area = nullptr;      // 中央滚动区
    JZVirtualScreenView* m_view = nullptr;   // 滚动区里的"屏幕"控件 (归滚动区所有)
};

int main(int argc, char* argv[])
{
    JZApplication app;

    VirtualDeviceApp w;
    w.show();

    return app.exec();
}
