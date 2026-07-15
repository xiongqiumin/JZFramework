#include "JZApplication.h"
#include "JZSimulator.h"

// JZ BLE Simulator: 启动 LVGL 界面, 监听来自 JZBleDriverSimulator 的 BLE 仿真请求
int main()
{
    JZApplication app;
    JZSimulator sim;
    sim.run(19123);
    return app.exec();
}
