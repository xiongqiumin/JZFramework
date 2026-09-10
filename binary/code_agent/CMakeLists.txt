
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
# 打包 JZ JS 引擎产物到 exe 同配置目录(bin/<CONFIG>/), 统一放进一个大的部署目录 jz_engine/:
#   - jz_engine/jz_types/ : 完整类型定义(供 initProject 拷进用户工程, 使工程自包含)
#   - jz_engine/jz_js/    : 运行时 JS(dist 编译产物: core/net/serialport), 供用户工程 import
# 用 js_wrapper/package.py 打包。脚本只清理它自己要写入的目标子目录(jz_types/jz_js),
# 不删其它任何东西(删除策略有路径守卫, 误删会直接报错), 避免旧 CMake 残留文件
# (如历史 core/ 副本) 与根目录声明撞出 TS2300 Duplicate identifier。
# 文件依赖参考 jz_add_resources: 显式 DEPENDS 实际的 dist/types 文件(存在才加),
# 改动 .ts 重新 tsc 后打包会自动重跑; 用 stamp 文件做增量标记。
# 注意: dist 由 `tsc -b js_wrapper` 生成(非 CMake target), 缺失时脚本跳过并告警。
# ---------------------------------------------------------------------------
set(JZ_WRAP_PY "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper/package.py")
set(JZ_JS_OUT "${CODE_AGENT_BIN_DIR}/$<CONFIG>")
set(JZ_TYPES_SRC "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/types")
set(JZ_WRAP_SRC "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper")
set(JZ_JS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/jz_js_packaged.stamp")

# 仅把已存在的 dist 加入依赖, 避免未跑 tsc 时 configure 报找不到文件
set(JZ_JS_DEPS "${JZ_WRAP_PY}" "${JZ_TYPES_SRC}")
foreach(pkg core net serialport)
    set(_d "${JZ_WRAP_SRC}/${pkg}/dist/index.js")
    if(EXISTS "${_d}")
        list(APPEND JZ_JS_DEPS "${_d}")
    endif()
endforeach()

add_custom_command(
    OUTPUT "${JZ_JS_STAMP}"
    COMMAND ${JZ_PYTHON_EXECUTABLE} "${JZ_WRAP_PY}"
            --out "${JZ_JS_OUT}" --types "${JZ_TYPES_SRC}" --js-wrapper "${JZ_WRAP_SRC}"
    COMMAND ${CMAKE_COMMAND} -E touch "${JZ_JS_STAMP}"
    DEPENDS ${JZ_JS_DEPS}
    COMMENT "Package JZ JS engine (types + runtime dist) next to code_agent binaries (per-config)")
add_custom_target(copy_jz_js ALL DEPENDS "${JZ_JS_STAMP}")
add_dependencies(code_agent copy_jz_js)
add_dependencies(code_runtime copy_jz_js)
add_dependencies(code_agent_test copy_jz_js)
