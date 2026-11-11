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

# JZSharedMemory 独立测试 (独立 exe): 含跨进程 attach(起自己当子进程)
add_executable(test_sharedmem
    test_sharedmem.cpp)
target_link_libraries(test_sharedmem JZFramework)

# ---- ctest 注册: 仅含通过判定的功能用例 (test_benchmark* 为无断言基准程序, 不注册) ----
add_test(NAME core.test_core COMMAND test_core)
set_tests_properties(core.test_core PROPERTIES TIMEOUT 600)

add_test(NAME core.test_process COMMAND test_process)
set_tests_properties(core.test_process PROPERTIES TIMEOUT 120)
