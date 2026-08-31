#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZWindow.h"
#include "JZTimer.h"
#include "JZMessageBox.h"
#include "JZDebug.h"
#include "main_window.h"

#include <string>

// --dialog-smoke: 自动验证对话框 (500ms 打开并 exec -> 700ms 后自动 OK -> 1600ms 退出)
static void startDialogSmoke(JZApplication& app, JZWindow& window)
{
    JZTimer::singleShot(500, [&window] {
        JZMessageBox* box = JZMessageBox::question(&window, "Confirm", "Dialog smoke test?");
        jzDebug() << "dialog opened, exec...";
        JZTimer::singleShot(700, [box] {
            if (box) box->accept();
            jzDebug() << "dialog accepted";
        });
        int r = box->exec();   // 模态阻塞, 由上面的 accept() 结束
        jzDebug() << "dialog result: " << r;
    });
    JZTimer::singleShot(1600, [&app] { app.exit(0); });
}

int app_main(int argc, char* argv[])
{
    JZApplication app;

    JZWindow window(800, 600, "JZWidget Widgets");
    MainWindow content(&window);
    content.show();

    if (argc > 1 && std::string(argv[1]) == "--dialog-smoke")
        startDialogSmoke(app, window);

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
