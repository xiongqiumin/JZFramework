#include "JZApplication.h"
#include "JZWindow.h"
#include "JZDebug.h"
#include "JZCrashReport.h"
#include "scroll_game.h"

#include <cstring>
#include <string>

// 横版滚屏示例 (test/game/scroll)
//   test_game_scroll.exe                 正常窗口, ←/→ 移动, +/- 缩放, R 重开
//   test_game_scroll.exe --auto          自动右跑, 打印 [scroll] 状态行, 到边界后判钳制并退出
//   test_game_scroll.exe --auto --shot out.png
//                                        自动跑 + 中途抓一张 PNG (自己看画面/给 agent 自查)
int app_main(int argc, char* argv[])
{
    bool autoMode = false;
    std::string shotPath;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--auto") == 0)
            autoMode = true;
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shotPath = argv[++i];
    }

    // 崩溃(自绘/回调里的访问违例)时把符号化堆栈 + minidump 落到 <exe目录>/crash/。
    // 自绘游戏崩了不会有任何提示, 没有这个文件很难定位; 示例里就开着。
    JZCrashReportManager::instance()->init(nullptr);

    JZApplication app;

    JZWindow window(ScrollGame::kViewW, ScrollGame::kViewH, "JZGame Scroll - JZGameView2D");
    window.setFixedSize(window.width(), window.height());

    ScrollGame game(&window);
    game.setGeometry(0, 0, window.width(), window.height());
    game.setAutoMode(autoMode);
    game.setShotPath(shotPath);
    game.show();

    window.show();
    return app.exec();
}

int main(int argc, char* argv[])
{
    return app_main(argc, argv);
}
