add_executable(sprite_bench sprite_bench.cpp)
# JZGame 单独 DLL (JZSprite/JZScene 的容量基准)
target_link_libraries(sprite_bench JZGame)
