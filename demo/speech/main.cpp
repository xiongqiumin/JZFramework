#include <string>
#include <vector>
#include "JZApplication.h"
#include "JZDebug.h"
#include "JZSpeech.h"

int main(int argc, char *argv[])
{
    JZApplication app;

    JZSpeech speech;

    if (!speech.init())
    {
        jzDebug() << "初始化失败";
        return 1;
    }

    return app.exec();
}
