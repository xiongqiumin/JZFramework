#include <QApplication>
#include <QTimer>

#include "JZApplication.h"
#include "JZSimulator.h"
#include "MainWindow.h"

// JZ BLE Simulator: Qt 作为主事件循环驱动界面, JZ 事件循环由定时器桥接
int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);    

    // Qt 应用: 作为主事件循环驱动界面
    QApplication app(argc, argv);

    // JZ 框架应用: 提供 jzApp 与事件循环 (网络事件派发)
    JZApplication jz_app;

    // 模型/控制器
    JZSimulator sim;
    // Qt 桥接器: 把模型事件以 Qt 信号派发到 UI 线程 (生命周期归属 Qt 对象树)
    JZSimulatorBridge bridge(&app);
    sim.setBridge(&bridge);
    // 视图: 构造时连接 bridge 信号并构建界面 (此时 GATT 已预置, 可读数据)
    MainWindow w(&sim, &bridge);
    // 启动网络监听
    sim.start(19123);
    w.show();

    // 用 Qt 定时器周期性驱动 JZ 事件循环, 派发网络(后台线程)投递到主线程的事件
    QTimer jzPump;
    QObject::connect(&jzPump, &QTimer::timeout, []() 
    { 
        jzApp->processEvents(); 
    });
    jzPump.start(20);

    return app.exec();
}
