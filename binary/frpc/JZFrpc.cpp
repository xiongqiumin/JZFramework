#include "JZApplication.h"
#include "JZFrpClient.h"
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
    
    string config_path = app.applicationDirPath() + "/jzfrpc.ini";
    if (argc > 1)
    {
        config_path = argv[1];
    }
    
    jzInfo() << "JZFrpClient starting...";
    jzInfo() << "Loading config from: " << config_path;
    
    JZSettings settings;
    if (!settings.open(config_path))
    {
        jzError() << "Failed to open config file: " << config_path;
        jzError() << "Usage: jzfrpc [config_file.ini]";
        return 1;
    }
    
    JZFrpClient frpc;
    JZFrpClientConfig config;
    
    config.serverIp = settings.getValue("serverAddr");
    config.serverPort = settings.getInt("serverPort", 7000);
    config.token = settings.getValue("token");
    if (config.serverIp.empty())
    {
        jzError() << "Error: server_ip is required in config";
        return 1;
    }
    
    for(int proxy_idx = 0; proxy_idx < 100; proxy_idx++)
    {
        string section_name = "proxy" + to_string(proxy_idx);
        if (!settings.hasGroup(section_name))
        {
            break;
        }
        
        settings.beginGroup(section_name);
        string name = settings.getValue("name", "proxy" + to_string(proxy_idx));
        string local_ip = settings.getValue("localIP", "127.0.0.1");
        int local_port = settings.getInt("localPort", 0);
        int remote_port = settings.getInt("remotePort", 0);
        settings.endGroup();
        
        if (local_port > 0 && remote_port > 0)
        {
            config.addProxy(name, local_ip, local_port, remote_port);
            jzInfo().noquote() << "Added proxy:" << name << ", remote" << remote_port << "->" << local_ip << local_port;
        }
        else
        {
            jzError() << "Invalid proxy config for " << section_name 
                        << ", skipping (local_port=" << local_port 
                        << ", remote_port=" << remote_port << ")";
            return 1;
        }
    }
    
    if (config.proxyList.empty())
    {
        jzError() << "Error: no valid proxy configuration found";
        return 1;
    }
    
    jzInfo() << "JZFrpClient connecting to server...";
    jzInfo() << "Server: " << config.serverIp << ":" << config.serverPort;
    
    frpc.setConfig(config);
    frpc.connectToServer();
    if(!frpc.waitForConnected(15 * 1000))
    {
        jzError() << "Failed to connect to server";
        return 1;
    }
    jzInfo() << "JZFrpClient connect to server successed";
    
    return app.exec();
}
