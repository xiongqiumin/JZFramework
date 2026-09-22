# JZMenu::exec 自测: 会真弹一层菜单(原生窗口 + 嵌套模态循环), 需要桌面会话,
# 故**不注册 ctest**(与 test_widget / test_shooter 那些"要看窗口"的样例同一处理)。
# 手动跑: build/test/widget/menu/Debug/test_menu_exec.exe (退出码非 0 = 有失败项)
add_executable(test_menu_exec test_menu_exec.cpp)
target_link_libraries(test_menu_exec JZFramework)
