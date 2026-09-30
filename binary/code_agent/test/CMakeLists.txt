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
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZEngineRuntime.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZSourceMap.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZSourceMap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZExeResource.cpp
    # computer use: JZEngineRuntime::initEngine 会挂载 __jz.computeruse ⇒ 这几个源文件
    # 必须一起编进来(它们不在 JZFramework.lib 里; 与 binary/code_agent/runtime/CMakeLists.txt 同一套依赖)。
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZScreenShot.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
)

add_executable(code_agent_test ${CODE_AGENT_TEST_SOURCES})
target_include_directories(code_agent_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_agent_test PRIVATE JZFramework)
# JZComputerUse 的虚拟桌面后端在 binary/virtual_device (见那里的 CMakeLists)
target_link_libraries(code_agent_test PRIVATE jz_virtual_device)

# 测试要"调用运行时"（code_runtime 子进程），保证其先构建
add_dependencies(code_agent_test code_runtime)
target_link_libraries(code_agent_test PRIVATE code_agent_core)

# ---------------------------------------------------------------------------
# code_agent_lsp_test - 直连 TSGO(tsc --lsp --stdio) LSP 接口冒烟测试。
# 在 tmp 下新建一个最小 TS 工程（tsconfig.json + sample.ts），逐个验证 10 个
# tool（definition/references/quickinfo/rename/completion/document_highlights/
# diagnostics/source/outline/members）的返回结构与定位是否正确。
# 仅依赖 JZTsLangServer + JZFramework，不引入 GUI / 大模型 / code_runtime。
# ---------------------------------------------------------------------------
set(CODE_AGENT_LSP_TEST_SOURCES
    lsp_test.cpp
)
add_executable(code_agent_lsp_test ${CODE_AGENT_LSP_TEST_SOURCES})
target_include_directories(code_agent_lsp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_agent_lsp_test PRIVATE JZFramework)
target_link_libraries(code_agent_lsp_test PRIVATE code_agent_core)

# ---------------------------------------------------------------------------
# taskstore_test - TaskHistoryManager（SQLite 任务库）独立测试。
# 自包含（无 GUI / 大模型 / 网络），库建在 applicationTmpPath() 下的临时目录，
# 覆盖建库、持久化、对话往返、删任务级联、工程过滤、旧 index.json 自动导入。
# ---------------------------------------------------------------------------
# TaskHistoryManager 属于 code_agent(exe) 侧源文件、不在 code_agent_core 库里，
# 这里与 main_test.cpp 列 JZEngineRuntime.cpp 的做法一致，直接把它编进测试。
add_executable(taskstore_test
    taskstore_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../TaskHistoryManager.cpp
)
target_include_directories(taskstore_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(taskstore_test PRIVATE JZFramework)
target_link_libraries(taskstore_test PRIVATE code_agent_core)

add_test(NAME code_agent.taskstore COMMAND taskstore_test)
set_tests_properties(code_agent.taskstore PROPERTIES TIMEOUT 120)

# ---------------------------------------------------------------------------
# appcontainer_demo - 验证 AppContainer「目录白名单」隔离（Windows 8+）。
#   裸 Win32 演示：建容器 SID -> 给受保护目录加允许 ACE -> 在容器内分别 del
#   受保护/外部目录的哨兵文件，断言「外不可删、内可删」。需要 userenv/aclapi/advapi32。
# ---------------------------------------------------------------------------
if(WIN32)
    add_executable(appcontainer_demo appcontainer_demo.cpp)
    target_link_libraries(appcontainer_demo PRIVATE userenv advapi32 JZFramework)
    target_link_libraries(appcontainer_demo PRIVATE JZFramework)
endif()