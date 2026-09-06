
# code_runtime - 已编译 TS 工程产物的隔离运行器（无 GUI）。
# 供 code_agent / code_agent_test 以子进程方式"调用运行时"：
#   用法: code_runtime <projectRoot>
# 主入口读取工程根 project.json 的 entry（先打包/发布才有；缺失或产物不存在报错退出）。
# 内部用 JZJSEngine 以模块方式加载并执行产物，出错时经 source map
# 把 JS 栈翻译回 TS 源码位置（JZEngineRuntime）。
set(CODE_RUNTIME_SOURCES
    main.cpp
    JZRuntimeDebugger.h
    JZRuntimeDebugger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZExeResource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZExeResource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.h
)

add_executable(code_runtime ${CODE_RUNTIME_SOURCES})
target_include_directories(code_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_runtime PRIVATE JZFramework)
