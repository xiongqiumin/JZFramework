# ---------------------------------------------------------------------------
# 发布 exe 的输出目录：统一在 <build>/binary/bin/<CONFIG>
#
# 只有**要发布的那些** exe 列在文件末尾那张清单里（它们 + 随附的 js_wrapper / ts_compiler /
# example / OpenCV 等 DLL 就是发版、拷机器要打包的一整套，见 tools/release/publish.py）。
# 测试 / 评测 exe（binary/agent_cli/test/agent_cli_test.cpp）**不进这张表**：
# 它们与 test/ 下的测试、tools/ 里的
# 开发工具一起，统一输出到 <build>/test/bin/<配置>（根 CMakeLists 的 JZ_TEST_BIN_DIR，
# 各自 CMakeLists 里显式 set_target_properties 一句）—— 整个构建树里 exe 只有
# "发布目录"与"test/bin"两个落点。
#
# ⚠ 别改成 CMAKE_RUNTIME_OUTPUT_DIRECTORY：那个会把本目录下**所有** exe 一起扫进来，
#   "发布"与"测试"就分不开了（那正是上一版的毛病）。
# ⚠ 变量 JZ_BINARY_BIN_DIR 留在这儿是给"目标之外"的地方推导该目录用的
#   （runtime 找 code_runtime.exe 旁的 ts_compiler、把 js_wrapper 打包进去），
#   必须在 add_subdirectory 之前定义，子目录才看得到。
# 配套的 add_custom_command 一律用 $<TARGET_FILE_DIR:目标>（自然跟着目标走），别硬编码路径。
# ---------------------------------------------------------------------------
set(JZ_BINARY_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
# 供 js_runtime/ 推导发布目录(找 code_runtime.exe 旁的 ts_compiler/tsc/)——
# 原来定义在 desktop/CMakeLists.txt, 迁移后提到这里, 所有子目录都能看到。
set(CODE_AGENT_BIN_DIR "${JZ_BINARY_BIN_DIR}")

# 通用 agent 工具库: 已迁到 agent_cli/tool/(target 名仍是 agent_tool, 全仓链接点不用改)。
# 第二个参数指定构建子目录名, 免得和 add_subdirectory(agent_cli) 的目录树混在一起。
add_subdirectory(agent_cli/tool agent_tool)
add_subdirectory(agent_cli)   # 内含 node/: 远端被控端(agent_node)与 cli 同属一个二进制族
add_subdirectory(frpc)

# jzpdf: JZDocument 的 PDF 后端(PDFium)。需要 -DJZ_PDFIUM_DIR=... , 没给就自己跳过。
add_subdirectory(jzpdf)

# code_agent 是自绘 UI 程序, 依赖 JZWidget(见顶层 CMakeLists: 目前仅 Windows 启用)
if(HAS_JZWidget)

# ---------------------------------------------------------------------------
# 部署 JZ JS 引擎产物(**js_wrapper/**, 一棵与源码同构的树)到上面那个**发布目录**。
#
# 为什么放在这里、只做一次: code_agent 与 virtual_device 现在输出到同一个目录, 若两边
# 各挂一个"往同目录写 js_wrapper"的自定义命令, 它们会互相拆台 —— package.py 每次都会先
# 整棵删掉再重铺(见它的"删除策略"), 并行构建下 A 刚铺完 B 就把它删了。
# 所以整个 binary/ 只留这一处, 谁需要谁 add_dependencies(<目标> copy_jz_js)。
#
# 内容与注意事项(dist 由 `tsc -b js_wrapper` 生成、文件依赖、GLOB 新包不一定当场生效等)
# 见 JZJSEngine/js_wrapper/package.py 与下面的注释。
# ---------------------------------------------------------------------------
set(JZ_WRAP_PY "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper/package.py")
set(JZ_JS_OUT "${JZ_BINARY_BIN_DIR}/$<CONFIG>")
set(JZ_WRAP_SRC "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper")
set(JZ_JS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/js_wrapper_packaged.stamp")

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
# ⚠ 新建一个包目录(index.ts / types/)之后, 上面的 GLOB **不一定当场生效** ——
#   实测(cv/onnx/ocr 三个包): 新包没进 DEPENDS ⇒ stamp 不失效 ⇒ exe 边根本没有那个包,
#   而 JS 侧 `import '新包'` 会**静默退出**(code_runtime 连报错都不打, 只给退出码 1, 很难查)。
#   遇到"新加的包没生效": 先 `cmake -S . -B build` 重新 configure; 还不行就删掉
#   ${JZ_JS_STAMP} 强制重跑打包。

add_custom_command(
    OUTPUT "${JZ_JS_STAMP}"
    COMMAND ${JZ_PYTHON_EXECUTABLE} "${JZ_WRAP_PY}"
            --out "${JZ_JS_OUT}" --js-wrapper "${JZ_WRAP_SRC}"
    COMMAND ${CMAKE_COMMAND} -E touch "${JZ_JS_STAMP}"
    DEPENDS ${JZ_JS_DEPS}
    COMMENT "Package JZ JS engine (js_wrapper/ tree) into the shared binary bin dir (per-config)")
add_custom_target(copy_jz_js ALL DEPENDS "${JZ_JS_STAMP}")

# ---------------------------------------------------------------------------
# 示例工程：随发布铺到 <exe>/example/ —— 拿到发布目录的人有能直接读、也能直接编译运行的
# 示例（类型/运行时用同级的 ../js_wrapper，编译器用同目录的 ts_compiler）。
# 源 = binary/js_runtime/<示例>（就是 code_runtime 的那几个示例/测试工程）。
# ⚠ 只铺**源码**：每个工程自己编译出来的 build/（tsconfig 的 outDir）不随发布走，
#   由 copy_examples.py 过滤掉；它顺带把 tsconfig 里那条"仓库相对的类型来源"
#   改写成部署布局的 ../../js_wrapper/...（仓库里那份源码不动）。
# 用 stamp 做增量：示例源码变了才重拷（依赖用 GLOB_RECURSE 收，排除 build/ 那类产物）。
# ---------------------------------------------------------------------------
set(JZ_EG_SRC "${JZFRAMEWORK_ROOT_DIR}/binary/js_runtime")
set(JZ_EG_PY "${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/copy_examples.py")
set(JZ_EG_OUT "${JZ_BINARY_BIN_DIR}/$<CONFIG>/example")
set(JZ_EG_STAMP "${CMAKE_CURRENT_BINARY_DIR}/examples_packaged.stamp")

set(JZ_EG_NAMES jsapi_example test_jswidget test_jsshooter bteserver gomoku)
set(JZ_EG_DEPS "${JZ_EG_PY}")
foreach(_eg ${JZ_EG_NAMES})
    file(GLOB_RECURSE _eg_files CONFIGURE_DEPENDS "${JZ_EG_SRC}/${_eg}/*")
    list(FILTER _eg_files EXCLUDE REGEX "[/\\\\](build|node_modules)[/\\\\]")
    list(APPEND JZ_EG_DEPS ${_eg_files})
endforeach()

add_custom_command(
    OUTPUT "${JZ_EG_STAMP}"
    COMMAND ${JZ_PYTHON_EXECUTABLE} "${JZ_EG_PY}" --src "${JZ_EG_SRC}" --dst "${JZ_EG_OUT}"
    COMMAND ${CMAKE_COMMAND} -E touch "${JZ_EG_STAMP}"
    DEPENDS ${JZ_EG_DEPS}
    COMMENT "Copy example projects (source only) into the publish dir (per-config)")
add_custom_target(copy_examples ALL DEPENDS "${JZ_EG_STAMP}")

# 示例与 js_wrapper 同属"随发布内容"：谁要后者就把它也带上 ——
# publish.py 只编 code_agent / code_runtime 两个目标（不走 ALL），靠这条才能带出来。
add_dependencies(copy_jz_js copy_examples)

add_subdirectory(updater)
# virtual_device: 虚拟桌面(虚拟显示器 + 窗口管理器) + 它的宿主程序。
# 同时提供静态库 jz_virtual_device 给 CU / 测试链 (见 binary/virtual_device/CMakeLists.txt)。
add_subdirectory(virtual_device)
# 2026-11-13 合并: 原 binary/code_agent 拆成 js_runtime/(提到 binary 顶层, code_runtime)
#   · agent_cli/desktop/(桌面壳) · agent_cli/benchmark/(只剩评测集 data/, 由 agent_cli_test 复用)。
# 顺序有依赖: virtual_device -> js_runtime -> desktop。
# ⛔ code_agent_benchmark 已删除(2026-11-13): 评测统一到 binary/agent_cli/test/agent_cli_test。
add_subdirectory(js_runtime)
add_subdirectory(agent_cli/desktop code_agent_desktop)
endif()

# ---------------------------------------------------------------------------
# 发布清单：上面这些子目录里"要发布的 exe"，显式指定输出到 <build>/binary/bin/<CONFIG>。
#
# 新加一个**发布** exe：把名字加进这张表；不加就是"测试/评测 exe"，走各子目录的默认位置
# （<build>/binary/<子目录>/<配置>/），发布目录里不会出现它。
# ⚠ 必须写在创建它们的 add_subdirectory **之后**（属性随时可设，但目标得先存在）；
#   if(TARGET) 兜住"本次配置没建的"（jzpdf 要 -DJZ_PDFIUM_DIR，virtual_device 要 HAS_JZWidget）。
# ⚠ agent_cli_server 列在这张表里是因为它默认到**自己 exe 同目录**去找 agent_cli 拉起
#   （见 JZAgentCliServer::resolveProgram），两者必须同目录。
# ---------------------------------------------------------------------------
foreach(t code_agent code_runtime virtual_device agent_cli agent_cli_server agent_node
          jzfrpc jzfrps jzupdater jzpdf)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${JZ_BINARY_BIN_DIR}/$<CONFIG>")
    endif()
endforeach()
