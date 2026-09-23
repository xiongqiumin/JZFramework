
# code_runtime - 跑一份 JS 的隔离运行器（无 GUI）。
# 供 code_agent / code_agent_test 以子进程方式"调用运行时"：
#   用法: code_runtime --entry <file.js>   （以该文件所在目录为运行根/工作目录）
#         code_runtime                     （运行根 = 当前工作目录，主入口 = 下面的 main.js）
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

# ASan 构建: 把 ASan 运行库拷到 exe 边上(见根 CMakeLists 的 JZ_ENABLE_ASAN)
jz_copy_asan_runtime(code_runtime)

# ---------------------------------------------------------------------------
# jsapi_example: 一份脚本一个主题 的 JS API 例子(读者=模型/初学者)
#
# 两个作用:
#   ① 文档 —— '--entry <这份 js>' 就能跑, 每条语句旁边写了"这行在干嘛/边界在哪";
#   ② 回归 —— 每个例子自带自检(helper/assert.js), 断言不成立就 throw =>
#             code_runtime 退出码非 0 => 下表的 ctest 变红。
# 例子直接用 source dir(不放进 build): 改了脚本下一次跑就是新的。
#
# ⚠ GLOB 在 configure 时求值 —— 新加一份例子要重新配一次 CMake(或动一下本文件)
#   才会出现对应测试; 改例子内容不需要。helper/ 是共用脚本(不是例子), 只收顶层 *.js。
# ---------------------------------------------------------------------------
if(BUILD_TEST)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/jsapi_example" JZ_JSAPI_EXAMPLE_DIR)
    file(GLOB JZ_JSAPI_EXAMPLES "${JZ_JSAPI_EXAMPLE_DIR}/*.js")
    foreach(JZ_EXAMPLE ${JZ_JSAPI_EXAMPLES})
        get_filename_component(JZ_EXAMPLE_NAME "${JZ_EXAMPLE}" NAME_WE)
        add_test(NAME jsapi_example.${JZ_EXAMPLE_NAME}
                 COMMAND code_runtime --entry ${JZ_EXAMPLE})
        set_tests_properties(jsapi_example.${JZ_EXAMPLE_NAME} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
