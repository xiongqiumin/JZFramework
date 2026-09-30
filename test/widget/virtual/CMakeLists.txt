add_executable(test_virtual_device main.cpp)
target_link_libraries(test_virtual_device JZFramework)

# 不创建真实窗口 => 可以进 ctest
add_test(NAME virtual_device COMMAND test_virtual_device)
