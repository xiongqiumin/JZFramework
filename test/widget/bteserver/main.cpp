// ---------------------------------------------------------------------------
// test_bteserver —— C++ 版 bteserver(见 BTEServer.h 的说明)。
//
//   test_bteserver            开界面跑事件循环(默认, 与原版一样最大化)
//   test_bteserver --dump     开界面 + 泵几轮事件后打印控件几何(排查布局用)
//   test_bteserver --clipcheck 同 --dump, 但故意把卡片容器摆到父控件外(验证裁剪), 留着窗口截图
//   test_bteserver --port N   换监听端口(默认 8001, 与原版一致)
// ---------------------------------------------------------------------------
#include <chrono>
#include <thread>

#include "JZApplication.h"
#include "BTEServer.h"

#include "JZMainWindow.h"
#include "JZLayout.h"
#include "JZScrollArea.h"
#include "JZScrollBar.h"
#include "JZListWidget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>

// 控件几何(窗口坐标)诊断: --dump 用。定位"越界/错位"这类布局问题就靠它。
static void dumpGeometry(BTEServer& ui)
{
    printf("\n=== 控件几何(窗口坐标) ===\n");
    auto dump = [](const char* what, JZWidget* w) {
        if (!w)
        {
            printf("  %-20s (null)\n", what);
            return;
        }
        const JZPoint at = w->mapTo(nullptr, JZPoint(0, 0));
        const JZSize sz = w->size();
        printf("  %-20s x=%-5d y=%-5d w=%-5d h=%-5d%s\n", what, at.x(), at.y(),
               sz.width(), sz.height(), w->isVisible() ? "" : "  [隐藏]");
    };

    dump("main(窗口)", ui.main);
    dump("centralWidget", ui.gen.centralWidget);
    dump("splitter", ui.gen.splitter);
    dump("widgetLeft", ui.gen.widgetLeft);
    dump("widgetMain", ui.gen.widgetMain);
    dump("labelDevice", ui.gen.labelDevice);
    dump("labelChannel", ui.gen.labelChannel);
    dump("tabWidget", ui.gen.tabWidget);
    dump("tabDevice(页)", ui.gen.tabDevice);
    dump("listWidget", ui.gen.listWidget);
    if (ui.listWidget)
        printf("    -> count=%d sizeHint=%dx%d viewport=%dx%d content=%dx%d\n",
               ui.listWidget->count(),
               ui.listWidget->sizeHint().width(), ui.listWidget->sizeHint().height(),
               ui.listWidget->viewportWidth(), ui.listWidget->viewportHeight(),
               ui.listWidget->contentWidth(), ui.listWidget->contentHeight());
    dump("stackedWidget", ui.gen.stackedWidget);
    dump("pageNormal", ui.gen.pageNormal);
    dump("cardScroll", ui.gen.cardScroll);
    if (ui.gen.cardScroll)
    {
        JZScrollArea* sa = ui.gen.cardScroll;
        const JZSize mn = ui.gen.itemWidgets ? ui.gen.itemWidgets->minimumSizeHint() : JZSize();
        const JZSize sh = ui.gen.itemWidgets ? ui.gen.itemWidgets->sizeHint() : JZSize();
        printf("    -> widgetResizable=%d content=%dx%d viewport=%dx%d 横条=%d 纵条=%d maxX=%d\n",
               (int)sa->widgetResizable(), sa->contentWidth(), sa->contentHeight(),
               sa->viewportWidth(), sa->viewportHeight(),
               sa->horizontalScrollBar() ? (int)sa->horizontalScrollBar()->isVisible() : -1,
               sa->verticalScrollBar() ? (int)sa->verticalScrollBar()->isVisible() : -1,
               sa->maxScrollX());
        printf("    -> sa->widget()=%p, gen.itemWidgets=%p, 同一个=%d, 它的 sizeHint/minsize=%dx%d / %dx%d\n",
               (void*)sa->widget(), (void*)ui.gen.itemWidgets,
               (int)(sa->widget() == ui.gen.itemWidgets),
               sh.width(), sh.height(), mn.width(), mn.height());
    }
    dump("itemWidgets(内容)", ui.gen.itemWidgets);
    dump("textLog", ui.gen.textLog);
    dump("groupDevice", ui.gen.groupDevice);
    dump("  lblDev", ui.gen.lblDev);
    dump("  boxDevice", ui.gen.boxDevice);
    dump("  lineQueryUnit", ui.gen.lineQueryUnit);
    dump("  lineQueryChannel", ui.gen.lineQueryChannel);
    dump("  dateFrom", ui.gen.dateFrom);
    dump("  dateTo", ui.gen.dateTo);
    dump("btnNone", ui.gen.btnNone);
    dump("lineNearDay", ui.gen.lineNearDay);
    dump("tableQuery", ui.gen.tableQuery);

    printf("  --- 通道卡片(前 10 张 + 可见性) ---\n");
    for (size_t i = 0; i < ui.channelCards.size() && i < 10; ++i)
        dump(("card[" + std::to_string(i) + "]").c_str(), ui.channelCards[i]);
    if (ui.gen.itemWidgets)
    {
        if (JZGridLayout* g = dynamic_cast<JZGridLayout*>(ui.gen.itemWidgets->layout()))
            printf("    -> itemWidgets grid: count=%d spacing=%d 行=%d 列=%d\n",
                   g->count(), g->spacing(), g->rowCount(), g->columnCount());
        else
            printf("    -> itemWidgets 没挂 JZGridLayout!\n");
    }
    printf("\n");
}

int app_main(int argc, char* argv[])
{
    JZApplication app;

    bool dumpMode = false;
    bool clipCheckMode = false;
    int port = 8001;                  // 原版端口; --port N 可换(0 会被引擎拒绝, 见 BTEServer.h)
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--dump")
            dumpMode = true;
        else if (a == "--clipcheck")
            clipCheckMode = true;
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    BTEServer ui;                  // 由本函数持有: 网络回调会长期引用它(见 BTEServer.h 的说明)
    buildBTEServer(ui, port);      // 工程目录默认当前目录下的 bte_project
    if (!ui.main)
    {
        printf("[FAIL] 建界面失败: %s\n", ui.error.c_str());
        return 1;
    }

    if (dumpMode || clipCheckMode)
    {
        // 布局是 show 之后才落位的 ⇒ 开出来、泵几轮事件, 再把几何打出来
        ui.main->show();
        ui.main->showMaximized();

        if (clipCheckMode)
        {
            // 裁剪验证: 故意把卡片容器摆到父控件(pageNormal)外面(左移 + 加宽, 会压到左栏),
            // 父控件默认就裁剪子控件 ⇒ 左栏应该完全看不到卡片。
            if (JZWidget* iw = ui.gen.itemWidgets)
                iw->setGeometry(-300, 52, 1400, 400);
            printf("[裁剪验证] 已把 itemWidgets 故意摆到父控件外: x=-300 w=1400(父宽 1136)\n");
        }

        for (int i = 0; i < 6; ++i)
        {
            app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        dumpGeometry(ui);

        if (clipCheckMode)
            return app.exec();       // 留着窗口给外部截图
        return 0;
    }

    // 与原版一致: 最大化跑(8 列卡片要有宽度)。⚠ 引擎的最小/最大化/还原都要求原生窗口
    // 已经建出来(它们是直接 ShowWindow), 所以先 show() 再 showMaximized()。
    ui.main->show();
    ui.main->showMaximized();
    const int rc = app.exec();
    delete ui.gen.root;
    return rc;
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
