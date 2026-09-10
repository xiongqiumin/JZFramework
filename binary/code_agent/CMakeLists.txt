
set(CODE_AGENT_CORE_SOURCES
    JZFileWatch.h
    JZFileWatch.cpp
    JZCodeTools.h
    JZCodeTools.cpp
    JZTsLangServer.h
    JZTsLangServer.cpp
    JZTsLangTools.h
    JZTsLangTools.cpp
    JZTsEditTools.h
    JZTsEditTools.cpp
    JZCodeAgent.h
    JZCodeAgent.cpp    
    JZExeResource.h
    JZExeResource.cpp
)
add_library(code_agent_core STATIC ${CODE_AGENT_CORE_SOURCES})
target_link_libraries(code_agent_core PRIVATE JZFramework)

set(CODE_AGENT_SOURCES
    main.cpp
    MainWindow.h
    MainWindow.cpp
    AppConfig.h
    SettingsDialog.h
    SettingsDialog.cpp
    TaskPanel.h
    TaskPanel.cpp
    TaskHistoryManager.h
    TaskHistoryManager.cpp
    TokenRing.h
    TokenRing.cpp
    ChatView.h
    ChatView.cpp    
    JZEngineRuntime.h
    JZEngineRuntime.cpp
    JZSourceMap.h
    JZSourceMap.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)
target_link_libraries(code_agent PRIVATE code_agent_core)

add_subdirectory(runtime)
add_subdirectory(test)
add_subdirectory(benchmark)

# ---------------------------------------------------------------------------
# 所有 code_agent 下的 exe 按构建配置分别输出到 bin/<CONFIG> 子目录
# （Debug -> bin/Debug，Release -> bin/Release，RelWithDebInfo/MinSizeRel 同理），
# 区分 debug/release。
# 每个配置子目录都是自包含的：exe + 随附的 jz_types + benchmark 的 data 都在同一
# 目录，initProject 用 applicationDirPath() 能正确定位到配套的 jz_types，benchmark
# 用 $<TARGET_FILE_DIR:...> 也能跟到对应配置。
# 用 $<CONFIG> 生成器表达式，对单配置 / 多配置生成器都自动按配置分目录。
# ---------------------------------------------------------------------------
set(CODE_AGENT_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(t code_agent code_runtime code_agent_test code_agent_lsp_test code_agent_benchmark)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CODE_AGENT_BIN_DIR}/$<CONFIG>")
endforeach()

# ---------------------------------------------------------------------------
# 把 JZJSEngine 的完整类型定义安装到 exe 目录（bin/<CONFIG>/jz_types/，含
# net/http/fetch/websocket 等）。这些声明会与 lib.dom.d.ts 冲突的全局
# fetch/WebSocket，需 tsconfig 设 "lib": ["ES2022"]（不带 DOM）才不会冲突。
# initProject 会把这份 jz_types 拷进新建工程（<工程根>/jz_types），生成的 tsconfig
# 用相对路径 include 它，工程因此自包含。（只保留根目录这份完整集，不再复制
# core/ 子集，避免重复。）
# 注意：必须装到 bin/<CONFIG>/jz_types，与 exe 同一配置子目录——initProject 用
# applicationDirPath()+"/jz_types" 当拷贝源，装错目录会找不到类型。
# ---------------------------------------------------------------------------
add_custom_target(copy_jz_types ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CODE_AGENT_BIN_DIR}/$<CONFIG>/jz_types"
    # 清掉旧 CMake 残留的 core/ 子目录（copy_directory 不会自动删除目标里多余文件，
    # 否则历史 core/ 副本会与根目录同名声明撞出 TS2300 Duplicate identifier）。
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CODE_AGENT_BIN_DIR}/$<CONFIG>/jz_types/core"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types" "${CODE_AGENT_BIN_DIR}/$<CONFIG>/jz_types"
    COMMENT "Install JZ JS engine type definitions next to code_agent binaries (per-config)")
add_dependencies(code_agent copy_jz_types)
add_dependencies(code_runtime copy_jz_types)
add_dependencies(code_agent_test copy_jz_types)
