add_executable(test_uiloader main.cpp)
target_link_libraries(test_uiloader JZFramework)

# 不开窗口(除非 --show) ⇒ 能进 ctest
add_test(NAME uiloader COMMAND test_uiloader)
