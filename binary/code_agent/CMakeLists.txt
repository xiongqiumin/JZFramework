
set(CODE_AGENT_CORE_SOURCES
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
# 通用工具在 binary/agent_tool 单独一个库(供远端 agent_node 复用同一批实现)。
# 这里必须 PUBLIC: ICodeToolHost 出现在 JZCodeAgent.h 的接口上(基类列表),
# 所有链 code_agent_core 的目标都要能看到 agent_tool 的 include 目录。
target_link_libraries(code_agent_core PUBLIC agent_tool)
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
    NavPanel.h
    NavPanel.cpp
    TaskHistoryManager.h
    TaskHistoryManager.cpp
    TokenRing.h
    TokenRing.cpp
    ChatView.h
    ChatView.cpp    
    JZScreenShot.h
    JZScreenShot.cpp
    JZEngineRuntime.h
    JZEngineRuntime.cpp
    JZSourceMap.h
    JZSourceMap.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)
target_link_libraries(code_agent PRIVATE code_agent_core)

# exe 统一输出到 <build>/binary/code_agent/bin/<CONFIG>（说明见下面那段注释）。
# ⚠ 必须**在子目录 add_subdirectory 之前**定义: 子目录(runtime 的 jsapi_example 要取
#   code_runtime.exe 旁的 ts_compiler/tsc/)靠它推导 bin/<配置>/ 下的路径 —— 晚定义的话
#   子目录作用域里看不到这个变量。
set(CODE_AGENT_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")

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
# (变量 CODE_AGENT_BIN_DIR 已在上面定义 —— 目标是在子目录里创建的, 所以这一轮
#  set_target_properties 必须在子目录之后。)
# ---------------------------------------------------------------------------
foreach(t code_agent code_runtime code_agent_test code_agent_lsp_test code_agent_benchmark)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CODE_AGENT_BIN_DIR}/$<CONFIG>")
endforeach()

# ASan 构建: 把 clang_rt.asan*_dynamic-x86_64.dll 拷到 exe 边上(MSBuild 不会自动拷)。
# 注意 add_custom_command(TARGET) 必须在"创建该 target 的目录"里调用 —— 各 exe 的调用点
# 分别在 runtime/ · test/ · benchmark/ 自己的 CMakeLists 里(见根 CMakeLists 的 JZ_ENABLE_ASAN)。
jz_copy_asan_runtime(code_agent)

# ---------------------------------------------------------------------------
# 打包 JZ JS 引擎产物到 exe 同配置目录(bin/<CONFIG>/), 统一放进一个大的部署目录 jz_engine/:
#   - jz_engine/jz_types/ : 完整类型定义(供 initProject 拷进用户工程, 使工程自包含)
#   - jz_engine/jz_js/    : 运行时 JS(各内置名一个目录: http/fs/net/serialport/... 的 dist), 供用户工程 import
# 类型不再有独立的 JZJSEngine/types 源目录, 单一来源就是 js_wrapper 自身:
#   - js_wrapper/types/       只有跨模块的真全局声明(console/fetch/URL/WebSocket/EventEmitter)
#   - js_wrapper/<pkg>/types/ 各包类型 + 该包裸露模块名的 ambient 声明(ambient.d.ts)
# 打包时按同构布局汇总到 jz_types/(types/ + <pkg>/types/), 使源码里的相对引用(../../types/、
# ambient.d.ts 的 ./<module>)在部署产物里原样可解析。
# 用 js_wrapper/package.py 打包。脚本只清理它自己要写入的目标子目录(jz_types/jz_js),
# 不删其它任何东西(删除策略有路径守卫, 误删会直接报错), 避免旧 CMake 残留文件
# (如历史 core/ 副本) 与根目录声明撞出 TS2300 Duplicate identifier。
# 文件依赖参考 jz_add_resources: 显式 DEPENDS 实际的 dist/types 文件(存在才加),
# 改动 .ts 重新 tsc 后打包会自动重跑; 用 stamp 文件做增量标记。
# 注意: dist 由 `tsc -b js_wrapper` 生成(非 CMake target), 缺失时脚本跳过并告警。
# ---------------------------------------------------------------------------
set(JZ_WRAP_PY "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper/package.py")
set(JZ_JS_OUT "${CODE_AGENT_BIN_DIR}/$<CONFIG>")
set(JZ_WRAP_SRC "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper")
set(JZ_JS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/jz_js_packaged.stamp")

# 依赖: 脚本本身 + 共享声明 + 各包类型(存在才加, 避免文件缺失时 configure 报错)
set(JZ_JS_DEPS "${JZ_WRAP_PY}")
file(GLOB JZ_TYPE_FILES CONFIGURE_DEPENDS
     "${JZ_WRAP_SRC}/types/*.d.ts" "${JZ_WRAP_SRC}/types/*.ts"
     "${JZ_WRAP_SRC}/*/types/*.d.ts")
list(APPEND JZ_JS_DEPS ${JZ_TYPE_FILES})
# 把各包的 dist 入口加进依赖(GLOB 只返回已存在的文件, 所以未跑 tsc 时也不会 configure 报错)。
# ⚠ 不能写死包名清单: "一个名字 = 一个包目录"之后增删名字很频繁, 写死会出现
#   "改了某个包的 .ts, 但它的 dist 不在 DEPENDS 里 -> stamp 不失效 -> 部署不更新"。
file(GLOB JZ_JS_DIST_FILES CONFIGURE_DEPENDS "${JZ_WRAP_SRC}/*/dist/index.js")
list(APPEND JZ_JS_DEPS ${JZ_JS_DIST_FILES})

add_custom_command(
    OUTPUT "${JZ_JS_STAMP}"
    COMMAND ${JZ_PYTHON_EXECUTABLE} "${JZ_WRAP_PY}"
            --out "${JZ_JS_OUT}" --js-wrapper "${JZ_WRAP_SRC}"
    COMMAND ${CMAKE_COMMAND} -E touch "${JZ_JS_STAMP}"
    DEPENDS ${JZ_JS_DEPS}
    COMMENT "Package JZ JS engine (types + runtime dist) next to code_agent binaries (per-config)")
add_custom_target(copy_jz_js ALL DEPENDS "${JZ_JS_STAMP}")
add_dependencies(code_agent copy_jz_js)
add_dependencies(code_runtime copy_jz_js)
add_dependencies(code_agent_test copy_jz_js)
