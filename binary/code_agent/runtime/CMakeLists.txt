
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
    # computer use：__jz.computeruse 的底层实现(见 JZEngineRuntime 的挂载点)。
    # JZComputerUse 要 JZScreenShot(抓屏/编码) ⇒ runtime 这边也得把它编进来。
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZScreenShot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZScreenShot.h
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.h
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.h
    # OpenCV 图像处理桥（__jz.cv）：JZEngineRuntime 会挂载它 ⇒ 必须跟着一起编。
    # 下面 HAS_JZCv 段里的 jz_opencv_enable 给的就是 JZ_CU_OPENCV 宏, 所以是"编入 OpenCV"的版本。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSCvModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSCvModule.h
    # ONNX Runtime 推理桥（__jz.onnx）：同样由 JZEngineRuntime 挂载 ⇒ 必须跟着编。
    # 编入 ORT 与否由下面的 jz_onnx_enable 决定（它独立于 JZCv）。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSOnnxModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSOnnxModule.h
    # 音频录放桥（__jz.audio）：JZEngineRuntime 会挂载 ⇒ 必须跟着编(JZAudio 本体在 JZFramework 库里)。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSAudioModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSAudioModule.h
)

add_executable(code_runtime ${CODE_RUNTIME_SOURCES})
target_include_directories(code_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_runtime PRIVATE JZFramework)
# JZComputerUse 的虚拟桌面后端在 binary/virtual_device (见那里的 CMakeLists)
target_link_libraries(code_runtime PRIVATE jz_virtual_device)

# computer use 的**像素族**后端: 有 JZCv(即 OpenCV + ONNX Runtime 齐)就把它们都开起来。
#   JZ_CU_OPENCV=1 -> matchImage / matchColor (模板匹配 / 颜色查找)
#   JZ_CU_OCR=1    -> matchText / ocrTexts (本地 PP-OCR), 即 match({kind:'ocr', text})
# 缺依赖时这几个接口一律返回"未找到", 不影响其它功能(退化路径必须显式)。
if(HAS_JZCv)
    target_link_libraries(code_runtime PRIVATE JZCv)
    target_compile_definitions(code_runtime PRIVATE JZ_CU_OCR=1)
    jz_opencv_enable(code_runtime)   # 头/库 + 按配置拷 opencv_world DLL
endif()

# ONNX Runtime：__jz.onnx 用它跑**任意** .onnx 模型（JZ_ONNX=1 + 头/库 + 拷 onnxruntime DLL）。
# ⚠ 独立于 JZCv 判断：只要装了 ORT 就该接上 —— OCR 需要它，但"脚本跑自己的模型"不需要 OCR。
jz_onnx_enable(code_runtime)

# 游戏模拟器控件(见 JZEmulator/)：接了之后 JS 侧才能 `new JZEmulatorView(...)`
# （JZEngineRuntime 里有 JZ_HAS_EMULATOR 守卫的注册调用），顺带把 mgba_host.dll 与它的
# MinGW 运行库拷到 exe 边上。没装那个模块就不接，JS 里 new 会抛清晰错误。
if(HAS_JZEmulator)
    jz_emulator_enable(code_runtime)
endif()

# ASan 构建: 把 ASan 运行库拷到 exe 边上(见根 CMakeLists 的 JZ_ENABLE_ASAN)
jz_copy_asan_runtime(code_runtime)

# ---------------------------------------------------------------------------
# jsapi_example: 一份脚本一个主题 的 JS API 例子(读者=模型/初学者)
#
# 两个作用:
#   ① 文档 —— 编译后 '--entry <build/那份 js>' 就能跑, 每条语句旁边写了"这行在干嘛/边界在哪";
#   ② 回归 —— 每个例子自带自检(helper/assert.ts), 断言不成立就 throw =>
#             code_runtime 退出码非 0 => 下表的 ctest 变红。
#
# 例子是 **TypeScript 工程**(src/*.ts + tsconfig.json, 与 bteserver / test_jswidget 同一套骨架):
# 引擎只认 JS(quickjs 不做转译), 所以用例跑的是**编译产物** build/*.js —— 构建期由下面的
# custom target 调 tsc 生成(例子本身不拷进 build: 改了 .ts 下一次跑就是新的)。
#
# ⚠ GLOB 在 configure 时求值(CONFIGURE_DEPENDS 会自动重配) —— 新加一份例子
#   (src/10_xxx.ts) 会自己长出对应测试; helper/ 是共用脚本(不是例子), 只收 src 顶层 *.ts。
# ⚠ 编译器取 code_runtime.exe 同目录的 ts_compiler/tsc/tsc.exe(与 code_agent 同源);
#   那里没有的时候这批用例会被跳过(configure 时明确提示), 不静默丢 ——
#   要换别的编译器就 -DJZ_TS_TSC=<tsc.exe 全路径> 或设同名环境变量。
# ---------------------------------------------------------------------------
if(BUILD_TEST)
    file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/jsapi_example" JZ_JSAPI_EXAMPLE_DIR)
    file(GLOB JZ_JSAPI_EXAMPLES CONFIGURE_DEPENDS "${JZ_JSAPI_EXAMPLE_DIR}/src/*.ts")

    # 编译用的 tsc(tsgo): 就用 **code_runtime.exe 同目录**下的 ts_compiler/tsc/tsc.exe
    # —— 与 code_agent / MainWindow 的默认同源(applicationDirPath()/ts_compiler/tsc/tsc.exe),
    # 目录由父目录定死的 CODE_AGENT_BIN_DIR 得到: ${CODE_AGENT_BIN_DIR}/<配置>/ts_compiler/tsc/
    # (见 code_agent/CMakeLists.txt; 变量在 add_subdirectory 之前定义, 所以这里可见)。
    # 要换别的编译器: -DJZ_TS_TSC=<全路径> 或同名环境变量。
    set(JZ_TS_TSC "$ENV{JZ_TS_TSC}" CACHE FILEPATH "jsapi_example 编译用的 tsc/tsgo 全路径(留空 = 取 code_runtime.exe 旁的 ts_compiler/tsc/)")
    set(_jz_tsc "${JZ_TS_TSC}")
    if(_jz_tsc AND NOT EXISTS "${_jz_tsc}")
        message(WARNING "JZ_TS_TSC=${_jz_tsc} 不存在, 改用 code_runtime.exe 旁的 ts_compiler/tsc/")
        set(_jz_tsc "")
    endif()
    if(NOT _jz_tsc)
        # 配置期探测 —— 只是用来决定"要不要注册用例", 所以只做存在性检查:
        # 单配置生成器看 CMAKE_BUILD_TYPE, 多配置(VS/Xcode)逐个配置看。
        # ⚠ 实际执行时用 $<CONFIG> 展开(见下面 add_custom_command) —— 按当前配置取,
        #   不会 Debug/Release 错配。
        set(_jz_tsc_cfgs ${CMAKE_BUILD_TYPE} ${CMAKE_CONFIGURATION_TYPES})
        list(REMOVE_DUPLICATES _jz_tsc_cfgs)
        foreach(_cfg IN LISTS _jz_tsc_cfgs)
            if(_cfg AND EXISTS "${CODE_AGENT_BIN_DIR}/${_cfg}/ts_compiler/tsc/tsc.exe")
                set(_jz_tsc "${CODE_AGENT_BIN_DIR}/$<CONFIG>/ts_compiler/tsc/tsc.exe")
                break()
            endif()
        endforeach()
    endif()

    if(_jz_tsc)
        # src/**/*.ts -> build/**/*.js(+ .js.map, 子目录保持镜像结构); *.d.ts 不产出 js。
        file(GLOB_RECURSE JZ_JSAPI_EXAMPLE_SRCS CONFIGURE_DEPENDS
             "${JZ_JSAPI_EXAMPLE_DIR}/src/*.ts")
        set(JZ_JSAPI_EXAMPLE_OUTPUTS)
        foreach(_src ${JZ_JSAPI_EXAMPLE_SRCS})
            if(_src MATCHES "\\.d\\.ts$")
                continue()
            endif()
            file(RELATIVE_PATH _rel "${JZ_JSAPI_EXAMPLE_DIR}/src" "${_src}")
            string(REGEX REPLACE "\\.ts$" ".js" _rel "${_rel}")
            list(APPEND JZ_JSAPI_EXAMPLE_OUTPUTS
                 "${JZ_JSAPI_EXAMPLE_DIR}/build/${_rel}"
                 "${JZ_JSAPI_EXAMPLE_DIR}/build/${_rel}.map")
        endforeach()

        add_custom_command(
            OUTPUT ${JZ_JSAPI_EXAMPLE_OUTPUTS}
            COMMAND "${_jz_tsc}" -p "${JZ_JSAPI_EXAMPLE_DIR}/tsconfig.json"
            WORKING_DIRECTORY "${JZ_JSAPI_EXAMPLE_DIR}"
            DEPENDS ${JZ_JSAPI_EXAMPLE_SRCS} "${JZ_JSAPI_EXAMPLE_DIR}/tsconfig.json"
            COMMENT "Build jsapi_example (TypeScript -> build/*.js)"
            VERBATIM)
        add_custom_target(jsapi_example_ts ALL DEPENDS ${JZ_JSAPI_EXAMPLE_OUTPUTS})

        foreach(JZ_EXAMPLE ${JZ_JSAPI_EXAMPLES})
            if(JZ_EXAMPLE MATCHES "\\.d\\.ts$")   # globals.d.ts 是类型声明, 不是例子
                continue()
            endif()
            get_filename_component(JZ_EXAMPLE_NAME "${JZ_EXAMPLE}" NAME_WE)
            set(JZ_EXAMPLE_JS "${JZ_JSAPI_EXAMPLE_DIR}/build/${JZ_EXAMPLE_NAME}.js")
            add_test(NAME jsapi_example.${JZ_EXAMPLE_NAME}
                     COMMAND code_runtime --entry ${JZ_EXAMPLE_JS})
            # REQUIRED_FILES: 没先构建 jsapi_example_ts 时给一条明确的失败, 而不是"跑了个不存在的入口"
            set_tests_properties(jsapi_example.${JZ_EXAMPLE_NAME} PROPERTIES
                                 TIMEOUT 60 REQUIRED_FILES "${JZ_EXAMPLE_JS}")
        endforeach()
    else()
        message(STATUS "jsapi_example: code_runtime.exe 旁没有 ts_compiler/tsc/tsc.exe "
                       "(按 bin/<配置>/ts_compiler/tsc/ 找的), 跳过 TS 例子的编译与用例; "
                       "用 -DJZ_TS_TSC=<tsc.exe 全路径> 指定编译器即可恢复")
    endif()
endif()
