#include <QApplication>
#include <QTimer>

#include "JZApplication.h"
#include "JZSimulator.h"

// JZ BLE Simulator: 启动 Qt 界面, 监听来自 JZBleDriverSimulator 的 BLE 仿真请求
int main(int argc, char *argv[])
{
    // JZ 框架应用: 提供 jzApp 与事件循环 (网络事件派发)
    JZApplication jz_app;
    jzApp = &jz_app;

    // Qt 应用: 作为主事件循环驱动界面
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    JZSimulator sim;
    sim.run(19123);

    // 用 Qt 定时器周期性驱动 JZ 事件循环, 派发网络(后台线程)投递到主线程的事件
    QTimer jzPump;
    QObject::connect(&jzPump, &QTimer::timeout, []() { jzApp->processEvents(); });
    jzPump.start(10);

    return app.exec();
}
