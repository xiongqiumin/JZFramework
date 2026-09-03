#include "JZApplication.h"
#include "JZWindow.h"
#include "main_window.h"

int app_main(int argc, char* argv[])
{
    JZApplication app;

    MainWindow window;
    window.show();

    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
