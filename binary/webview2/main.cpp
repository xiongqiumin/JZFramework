#include "JZApplication.h"
#include "JZWebServer.h"
#include "JZWebView2.h"

int main(int argc, char* argv[])
{
    JZApplication a;

    JZWebServer server;

    JZWebView2 view;
    view.show("index.html");

    return a.exec();
}
