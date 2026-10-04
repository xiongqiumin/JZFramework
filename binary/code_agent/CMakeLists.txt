
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
    # 随包产物(js_wrapper/ code_runtime.exe ts_compiler/)的定位工具: JZCodeAgent 与
    # test/ · benchmark/ 那两个"不在统一 bin 目录"的 exe 共用(见该头文件)。
    JZBundlePaths.h
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
    # computer use：底层桌面语义 + __jz.computeruse 桥（JZEngineRuntime 里挂载）。
    # ⚠ 编译得放在这里(而不是 JZFramework 主体)：JZComputerUse 依赖 code_agent 的 JZScreenShot，
    #   且它对 JZWidget 的语义树(dumpSemanticTree)有依赖 —— 都是"宿主能力"。
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.h
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.h
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
    # OpenCV 图像处理桥（__jz.cv）：JZEngineRuntime 会挂载它 ⇒ 必须跟着一起编。
    # ⚠ 本目标没调 jz_opencv_enable, 所以这里编出来的是"未编入 OpenCV"的退化版(__jz.cv
    #   存在但 available() 为 false)。要给 code_agent 也开 OpenCV, 补一句
    #   jz_opencv_enable(code_agent) 即可(会在 exe 边多拷一份 opencv_world DLL)。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSCvModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSCvModule.h
    # ONNX Runtime 推理桥（__jz.onnx）：同上，JZEngineRuntime 会挂载。
    # ⚠ 本目标既没 jz_opencv_enable 也没 jz_onnx_enable ⇒ 编出来的是退化版。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSOnnxModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSOnnxModule.h
    # 音频录放桥（__jz.audio）：同上，JZEngineRuntime 会挂载。
    # JZAudio 本体在 JZFramework 库里(JZ_MODULE_LIST 里有它, HAS_JZAudio=ON), 这里只编这层 JS 桥。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSAudioModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSAudioModule.h
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)
target_link_libraries(code_agent PRIVATE code_agent_core)
# JZComputerUse 的虚拟桌面后端在 binary/virtual_device (见那里的 CMakeLists)
target_link_libraries(code_agent PRIVATE jz_virtual_device)

# exe 的输出目录不在这里定: **要发布的**那两个(code_agent / code_runtime)在
# binary/CMakeLists.txt 末尾那张"发布清单"里, 统一输出到 <build>/binary/bin/<CONFIG>;
# 本目录下的测试 exe(test/)与评测 exe(benchmark/) 不在清单里 —— 就落在各自目录的默认位置。
# 这里只是把**发布目录**记成变量, 供"目标之外"的地方推导路径用:
#   - runtime/ 的 jsapi_example 要取 code_runtime.exe 旁的 ts_compiler/tsc/(见那里的注释);
#   - js_wrapper 树的打包(--out 就是这个目录, 见 binary/CMakeLists.txt 的 copy_jz_js)。
# ⚠ 必须**在子目录 add_subdirectory 之前**定义: 子目录作用域里要能看到它。
set(CODE_AGENT_BIN_DIR "${JZ_BINARY_BIN_DIR}")

add_subdirectory(runtime)
add_subdirectory(test)
add_subdirectory(benchmark)

# ASan 构建: 把 clang_rt.asan*_dynamic-x86_64.dll 拷到 exe 边上(MSBuild 不会自动拷)。
# 注意 add_custom_command(TARGET) 必须在"创建该 target 的目录"里调用 —— 各 exe 的调用点
# 分别在 runtime/ · test/ · benchmark/ 自己的 CMakeLists 里(见根 CMakeLists 的 JZ_ENABLE_ASAN)。
jz_copy_asan_runtime(code_agent)

# JZ JS 引擎产物(部署的 js_wrapper/ 树)的部署不在这里:
# code_agent 与 virtual_device 现在输出到**同一个** exe 目录, 两处各写一份 js_wrapper 会
# 互相拆台(package.py 每次先整棵删掉再重铺), 所以整个 binary/ 只留一处 ——
# 见 binary/CMakeLists.txt 里的 copy_jz_js(含"dist 要先 tsc -b js_wrapper"等注意事项)。
# 这里只声明依赖: 这几个 exe 都要 exe 边有 js_wrapper 才能跑 TS。
add_dependencies(code_agent copy_jz_js)
add_dependencies(code_runtime copy_jz_js)
add_dependencies(code_agent_test copy_jz_js)
