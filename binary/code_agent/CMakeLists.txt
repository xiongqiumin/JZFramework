
set(CODE_AGENT_SOURCES
    main.cpp
    MainWindow.h
    MainWindow.cpp
    ChatView.h
    ChatView.cpp
    JZCodeTools.h
    JZCodeTools.cpp
    JZCodeAgent.h
    JZCodeAgent.cpp
    JZExeResource.h
    JZExeResource.cpp
    JZEngineRuntime.h
    JZEngineRuntime.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)

add_subdirectory(runtime)
add_subdirectory(test)

# ---------------------------------------------------------------------------
# 所有 code_agent 下的 exe 统一输出到同一个 bin 目录。
# 这样类型定义 / 运行时 code_runtime 都能与 code_agent 的 exe 放在一起，
# 也方便 initProject 用 applicationDirPath() 定位到随附的 jz_types。
# 同时把 RUNTIME_OUTPUT_DIRECTORY_<CONFIG> 也设为同一目录，抹平 MSVC 多配置的
# <CONFIG> 子目录（Debug/Release 都进同一个 bin，避免分散）。
# ---------------------------------------------------------------------------
set(CODE_AGENT_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(t code_agent code_runtime code_agent_test)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CODE_AGENT_BIN_DIR}")
endforeach()
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    foreach(t code_agent code_runtime code_agent_test)
        set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CODE_AGENT_BIN_DIR}")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# 把 JZJSEngine 的类型定义安装到 exe 目录：
#   bin/jz_types/        完整类型（含 net/http/fetch/websocket 等；这些会声明与
#                        lib.dom.d.ts 冲突的全局 fetch/WebSocket，需 tsconfig 设
#                        "lib": ["ES2022"]（不带 DOM）才不会冲突，供进阶用户手动引用）。
#   bin/jz_types/core/   仅 core 子集（common/fs/path/os/process），与默认 DOM lib
#                        无冲突，由 JZCodeAgent::initProject 自动加入 tsconfig 的
#                        include，让生成的工程直接能解析这些 ambient 模块。
# ---------------------------------------------------------------------------
add_custom_target(copy_jz_types ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CODE_AGENT_BIN_DIR}/jz_types"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types" "${CODE_AGENT_BIN_DIR}/jz_types"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CODE_AGENT_BIN_DIR}/jz_types/core"
    COMMAND ${CMAKE_COMMAND} -E copy "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types/common.d.ts"   "${CODE_AGENT_BIN_DIR}/jz_types/core/common.d.ts"
    COMMAND ${CMAKE_COMMAND} -E copy "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types/fs.d.ts"      "${CODE_AGENT_BIN_DIR}/jz_types/core/fs.d.ts"
    COMMAND ${CMAKE_COMMAND} -E copy "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types/path.d.ts"    "${CODE_AGENT_BIN_DIR}/jz_types/core/path.d.ts"
    COMMAND ${CMAKE_COMMAND} -E copy "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types/os.d.ts"      "${CODE_AGENT_BIN_DIR}/jz_types/core/os.d.ts"
    COMMAND ${CMAKE_COMMAND} -E copy "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types/process.d.ts" "${CODE_AGENT_BIN_DIR}/jz_types/core/process.d.ts"
    COMMENT "Install JZ JS engine type definitions next to code_agent binaries")
add_dependencies(code_agent copy_jz_types)
add_dependencies(code_runtime copy_jz_types)
add_dependencies(code_agent_test copy_jz_types)
