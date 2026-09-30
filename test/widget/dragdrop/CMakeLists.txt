# 拖放示例 (需要交互: 手动往窗口里拖图片/文件/文本), 不注册 ctest
add_executable(test_dragdrop main.cpp)
target_link_libraries(test_dragdrop JZFramework)
