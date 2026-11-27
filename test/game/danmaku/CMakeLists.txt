add_executable(test_game_danmaku main.cpp main_window.cpp danmaku_widget.cpp)
# JZGame 单独 DLL (JZGameView/JZScene/JZSprite)
target_link_libraries(test_game_danmaku JZGame)
