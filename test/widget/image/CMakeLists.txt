# 目标名不能叫 test_image: test/agent 下已有同名可执行目标 (CMP0002 全局唯一)。
add_executable(test_imageio test_image.cpp)
target_link_libraries(test_imageio JZFramework)
