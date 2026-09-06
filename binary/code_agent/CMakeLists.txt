
set(CODE_AGENT_CORE_SOURCES
    JZCodeTools.h
    JZCodeTools.cpp
    JZTsLangServer.h
    JZTsLangServer.cpp
    JZTsLangTools.h
    JZTsLangTools.cpp
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
    ChatView.h
    ChatView.cpp    
    JZEngineRuntime.h
    JZEngineRuntime.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)
target_link_libraries(code_agent PRIVATE code_agent_core)

add_subdirectory(runtime)
add_subdirectory(test)
add_subdirectory(benchmark)

# ---------------------------------------------------------------------------
# 所有 code_agent 下的 exe 统一输出到同一个 bin 目录。
# 这样类型定义 / 运行时 code_runtime 都能与 code_agent 的 exe 放在一起，
# 也方便 initProject 用 applicationDirPath() 定位到随附的 jz_types。
# 同时把 RUNTIME_OUTPUT_DIRECTORY_<CONFIG> 也设为同一目录，抹平 MSVC 多配置的
# <CONFIG> 子目录（Debug/Release 都进同一个 bin，避免分散）。
# ---------------------------------------------------------------------------
set(CODE_AGENT_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(t code_agent code_runtime code_agent_test code_agent_lsp_test code_agent_benchmark)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CODE_AGENT_BIN_DIR}")
endforeach()
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    foreach(t code_agent code_runtime code_agent_test code_agent_lsp_test code_agent_benchmark)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CODE_AGENT_BIN_DIR}")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 把 JZJSEngine 的完整类型定义安装到 exe 目录（bin/jz_types/，含 net/http/fetch/
# websocket 等）。这些声明会与 lib.dom.d.ts 冲突的全局 fetch/WebSocket，需 tsconfig
# 设 "lib": ["ES2022"]（不带 DOM）才不会冲突，供需要它们的用户手动 include。
# 生成的工程默认不引用它——默认 tsconfig 只用纯 ES2022 + 自身 src 模块。
# （只保留根目录这份完整集，不再复制 core/ 子集，避免重复。）
# ---------------------------------------------------------------------------
add_custom_target(copy_jz_types ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CODE_AGENT_BIN_DIR}/jz_types"
    # 清掉旧 CMake 残留的 core/ 子目录（copy_directory 不会自动删除目标里多余文件，
    # 否则历史 core/ 副本会与根目录同名声明撞出 TS2300 Duplicate identifier）。
    COMMAND ${CMAKE_COMMAND} -E remove_directory "${CODE_AGENT_BIN_DIR}/jz_types/core"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types" "${CODE_AGENT_BIN_DIR}/jz_types"
    COMMENT "Install JZ JS engine type definitions next to code_agent binaries")
add_dependencies(code_agent copy_jz_types)
add_dependencies(code_runtime copy_jz_types)
add_dependencies(code_agent_test copy_jz_types)
