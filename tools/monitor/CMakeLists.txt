# JZMonitor 工具: 复用主树 JZFramework 静态库 (不再独立构建)
set(MONITOR_SOURCES
    JZMonitor.h
    JZMonitor.cpp
    main.cpp
)

add_executable(JZMonitor ${MONITOR_SOURCES})
target_include_directories(JZMonitor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(JZMonitor PRIVATE JZFramework)

# 输出目录: 非发布 exe 统一到 <build>/test/bin/<配置>(根 CMakeLists 的 JZ_TEST_BIN_DIR)。
set_target_properties(JZMonitor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}/$<CONFIG>")
