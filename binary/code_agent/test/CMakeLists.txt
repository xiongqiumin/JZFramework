# code_agent_test - "JZCodeAgent 创建工程 -> 编译 -> 调用运行时" 端到端流程测试
# （headless、无 GUI、不用大模型：工程文件直接写入）。
#
# 被测全流程：
#   1. 创建工程  JZCodeAgent::initProject/openProject/probeProject，新建到
#               JZPlatform::applicationTmpPath() 下的临时目录；
#   2. 写文件    直接写入多文件 TS（main.ts 跨目录 import lib.ts/util/strings.ts）；
#   3. 编译      宿主侧真实 tsc -p tsconfig.json -> build/*.js(+.js.map)；
#   4. 调用运行时 内置引擎 JZJSEngine 以模块方式跑产物 + JZEngineRuntime 栈翻译，
#                并以子进程方式调用 code_runtime（隔离运行器）再跑一遍；
#   5. 多文件加载/错误栈翻译都要有产物、有效结果与 TS 源码位置可断言。
#
# 编译只需 code_agent 中与 GUI 无关的源文件（MainWindow/ChatView 不参与）。
# 进程调用用框架 JZProcess，本文件不 include <windows.h>。
set(CODE_AGENT_TEST_SOURCES
    main_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZCodeAgent.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZCodeTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.cpp
)

add_executable(code_agent_test ${CODE_AGENT_TEST_SOURCES})
target_include_directories(code_agent_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_agent_test PRIVATE JZFramework)

# 测试要"调用运行时"（code_runtime 子进程），保证其先构建
add_dependencies(code_agent_test code_runtime)
