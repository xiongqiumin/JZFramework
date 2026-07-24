#include "JZApplication.h"
#include "JZEventLoop.h"
#include "JZDisplayDevice.h"
#include "JZDebug.h"
#include "main_window.h"

int app_main(int argc, char* argv[])
{
    JZApplication app;

    JZDisplayDevice device(800, 600);
    MainWindow window(&device);
    window.show();

    JZEventLoop loop;
    loop.exec();

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
