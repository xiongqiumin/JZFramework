
# code_runtime - 已编译 TS 工程产物的隔离运行器（无 GUI）。
# 供 code_agent / code_agent_test 以子进程方式"调用运行时"：
#   用法: code_runtime [运行根]   （运行根 = main.js 所在目录，缺省 exe 所在目录）
# 运行根下的 main.js 是主入口；同目录的 project.json 只是补充信息（编译时间、
# 原路径地址），解析（栈翻译还原源路径）会用到它。
# 内部用 JZJSEngine 以模块方式加载并执行产物，出错时经 source map
# 把 JS 栈翻译回 TS 源码位置（JZEngineRuntime + JZSourceMap）。
set(CODE_RUNTIME_SOURCES
    main.cpp
    JZRuntimeDebugger.h
    JZRuntimeDebugger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZExeResource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZExeResource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZSourceMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZSourceMap.h
)

add_executable(code_runtime ${CODE_RUNTIME_SOURCES})
target_include_directories(code_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_runtime PRIVATE JZFramework)
