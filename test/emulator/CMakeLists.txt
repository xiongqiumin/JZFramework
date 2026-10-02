# JZEmulator 冒烟测试：真窗口里跑 mGBA（要 JZWidget + 外部宿主 DLL mgba_host.dll）
# 没启用 JZEmulator（多半是找不到那个 DLL）就整个跳过，不当失败。
if(NOT HAS_JZEmulator)
    message(STATUS "test/emulator: 跳过（没启用 JZEmulator —— 多半是找不到 mgba_host.dll，"
                   "见 JZEmulator/JZEmulator.cmake 的 JZ_MGBA_DIR）")
    return()
endif()

add_executable(test_emulator main.cpp)
target_link_libraries(test_emulator JZFramework)
jz_emulator_enable(test_emulator)

# 把默认 DLL 目录写进代码，省得每次敲路径（命令行参数仍可覆盖）
target_compile_definitions(test_emulator PRIVATE JZ_EMULATOR_DLL_DIR_DEFAULT="${JZ_MGBA_DIR}")

# ⚠ 这个用例会**弹一个真窗口**（480x320 跑几秒游戏再自己关），所以不进 ctest 默认集；
#   要跑就手动 `ctest -R gba` 或在构建目录直接执行 test_emulator。
add_test(NAME emulator.smoke COMMAND test_emulator)
set_tests_properties(emulator.smoke PROPERTIES TIMEOUT 60 DISABLED TRUE)
