#include "JZGuiApplication.h"
#include "JZSimulatorDevice.h"

// JZ BLE Simulator 设备端入口: 启动 LVGL 界面并监听来自 JZBleDriverSimulator 的连接。
int main()
{
    JZGUIApplication app;
    JZSimulatorDevice device;
    device.run(19123);
    return app.exec();
}
