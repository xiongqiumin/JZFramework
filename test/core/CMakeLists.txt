# 核心功能测试
add_executable(test_core
    test_core.cpp
    test_thread.cpp
    test_mutexpointer.cpp
    test_rwlock.cpp)
target_link_libraries(test_core JZFramework)

# 基础组件基准测试 (独立 exe)
add_executable(test_benchmark
    test_benchmark.cpp)
target_link_libraries(test_benchmark JZFramework)

# IO 相关基准测试 (独立 exe): msleep / JZTimer / JZFile / JZDir / md5
add_executable(test_benchmark_io
    test_benchmark_io.cpp)
target_link_libraries(test_benchmark_io JZFramework)

# JZProcess 独立测试 (独立 exe)
add_executable(test_process
    test_process.cpp)
target_link_libraries(test_process JZFramework)
