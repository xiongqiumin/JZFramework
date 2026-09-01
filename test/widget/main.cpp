#include "JZApplication.h"
#include "JZWindow.h"
#include "main_window.h"

int app_main(int argc, char* argv[])
{
    JZApplication app;

    MainWindow window;            // JZWindow subclass: the window itself
    window.resize(800, 600);
    window.setTitle("JZWidget Widgets");

    window.show();   // not shown = not displayed

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
