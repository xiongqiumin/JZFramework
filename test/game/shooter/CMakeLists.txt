add_executable(test_game_shooter main.cpp main_window.cpp shooter_widget.cpp)
# JZGame 单独 DLL (JZGameView): 它 PUBLIC 链 JZFramework, 不用再单独写框架
target_link_libraries(test_game_shooter JZGame)
