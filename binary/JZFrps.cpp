#include "JZApplication.h"
#include "JZFrpServer.h"
#include "JZSettings.h"
#include "JZDebug.h"
#include <iostream>
#include <string>

using namespace std;

int main(int argc, char *argv[])
{
    JZApplication app;
    JZNetPackInit();
    JZFrpPacketInit();
    
    string config_path = app.applicationDirPath() + "/jzfrps.ini";
    if (argc > 1)
    {
        config_path = argv[1];
    }
    
    jzInfo() << "JZFrpServer starting...";
    jzInfo() << "Loading config from: " << config_path;
    
    JZSettings settings;
    if (!settings.open(config_path))
    {
        jzError() << "Failed to open config file: " << config_path;
        jzError() << "Usage: jzfrps [config_file.ini]";
        return 1;
    }
    
    JZFrpServer frps;
    JZFrpServerConfig config;
    
    config.token = settings.getValue("token", "");
    int port = settings.getInt("bindPort", 7000);
    
    frps.setConfig(config);
    if (!frps.listen(port))
    {
        jzError() << "Failed to listen on port " << port;
        return 1;
    }
    
    jzInfo() << "JZFrpServer started successfully";
    jzInfo() << "Listening on port: " << port;
    jzInfo() << "Token configured: " << (config.token.empty() ? "no" : "yes");
    jzInfo() << "Press Ctrl+C to exit";
    
    return app.exec();
}
