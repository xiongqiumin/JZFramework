# 横版滚屏示例: 消费**单独的 JZGame.dll** (JZGame 自己 PUBLIC 链 JZFramework,
# 所以这里只写 JZGame 就能拿到框架的头/宏)。
add_executable(test_game_scroll main.cpp scroll_game.cpp)
target_link_libraries(test_game_scroll JZGame)
