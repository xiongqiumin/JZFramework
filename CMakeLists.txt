cmake_minimum_required(VERSION 3.10)
project(JZFramework C CXX ASM)

set(CMAKE_CXX_STANDARD 14)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)

    # -----------------------------------------------------------------------
    # 全局忽略的告警(整个工程 + 全部子目录; 要加一条就往下面那行里追加 /wdNNNN)
    #
    # C4251 / C4275: "导出类的成员 / 基类没有 dll 接口"(成员是 std::string / std::vector / std::map
    #   这类 STL 类型时必然出现)。动态库构建(Windows 默认 JZFRAMEWORK_DYNAMIC=ON)下, 框架里几百个
    #   导出类都会刷这两条 —— 编 DLL 时刷, 每个 include 这些头文件的 exe 也刷(⚠ /W3 起才报)。
    #   **同一套工具集 + 同一个动态 CRT(/MD) + 同一配置**下, STL 布局一致 ⇒ 这两条只是提醒,
    #   所以全局忽略(2026-10-14 定案: 不做 Pimpl, 把"必须同工具集重编"写成交付前提)。
    #   ⚠ 想再看见它们: 注释掉下面这行重新 configure; 只想看某个文件: 在那文件里
    #     先 #pragma warning(default:4251) / (default:4275)。
    # -----------------------------------------------------------------------
    add_compile_options(/wd4251 /wd4275)
endif()

# ---------------------------------------------------------------------------
# AddressSanitizer (MSVC: /fsanitize=address)
#
# 打开方式(建议单独一个构建目录, 别污染日常 build/):
#   cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DJZ_ENABLE_ASAN=ON
#   cmake --build build-asan --config Debug --target code_runtime
#
# MSVC 侧的限制(踩过的坑):
#   * **与 /RTC1 互斥**(MSVC 会直接报错)—— 下面会从 Debug 配置里摘掉 /RTC1; 顺带也就没有
#     "Stack around the variable 'xxx' was corrupted" 这类 /RTCs 检查了: 换成 ASan 报,
#     而且 ASan 会直接给出"哪个函数里的哪个变量、多大、被写越界多少", 定位快得多。
#   * 必须用**动态运行库**: /MDd(Debug) 或 /MD(Release)。本工程没改过默认值(=动态), 别加
#     -DCMAKE_MSVC_RUNTIME_LIBRARY=.../MT, 否则链接时报不兼容。
#   * 不支持 /GL(LTCG) 与 /clr; 所以 ASan 一般只跑 Debug(Release 若开了 IPO 记得关)。
#   * 需要 VS2019 16.9+/VS2022 的 x64 工具集; ARM/ARM64 不支持。
#   * 运行时 DLL(clang_rt.asan_dbg_dynamic-x86_64.dll)要跟 exe 同目录(见文末打印的提示路径)。
# ---------------------------------------------------------------------------
option(JZ_ENABLE_ASAN "Enable AddressSanitizer (MSVC /fsanitize=address)" OFF)

# ---------------------------------------------------------------------------
# PDF 后端(jzpdf): 指向 PDFium 预编译包解压目录(内含 include/ 与 lib/)。
# 留空则不构建 jzpdf —— JZDocument 的 docx/xlsx 照常可用, 只有 PDF 会报"找不到后端"。
#   cmake -S . -B build -DJZ_PDFIUM_DIR=E:/libs/pdfium/win-x64
# ---------------------------------------------------------------------------
set(JZ_PDFIUM_DIR "" CACHE PATH "PDFium prebuilt package root (with include/ and lib/); empty = no PDF backend")

# 给 exe 目标挂上"把 ASan 运行库拷到输出目录"(MSBuild 自己不做这件事)。
# 必须在"创建该 target 的目录"里调用(CMake 对 add_custom_command(TARGET) 的限制):
#   jz_copy_asan_runtime(code_runtime)
function(jz_copy_asan_runtime target)
    if (NOT JZ_ENABLE_ASAN OR NOT MSVC OR NOT JZ_ASAN_RUNTIME_DLLS)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JZ_ASAN_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copy ASan runtime next to ${target}")
endfunction()

if (JZ_ENABLE_ASAN)
    if (NOT MSVC)
        message(WARNING "JZ_ENABLE_ASAN: 目前按 MSVC 写的; 其它编译器请自行加 -fsanitize=address")
    else()
        add_compile_options(/fsanitize=address)

        # /RTC1 与 /fsanitize=address 冲突: 从 Debug 配置的编译开关里摘掉(缓存改掉, 子目录才拿得到)
        foreach (_jz_flag CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
            string(REPLACE "/RTC1" "" _jz_val "${${_jz_flag}}")
            string(STRIP "${_jz_val}" _jz_val)
            set(${_jz_flag} "${_jz_val}" CACHE STRING "" FORCE)
        endforeach()

        # ASan 需要关掉增量链接(否则链接器会忽略/告警)
        add_link_options(/INCREMENTAL:NO)

        message(STATUS "JZ_ENABLE_ASAN = ON (MSVC /fsanitize=address; 已从 Debug 配置移除 /RTC1)")

        # ASan 运行库(cl.exe 同目录): exe 边上必须有它, 否则直接起不来(0xC0000135 缺 DLL)。
        # /MDd 的导入表里要的是 clang_rt.asan_dynamic-x86_64.dll, /MD 是同一个名字 —— 两个都拷最省心。
        get_filename_component(_jz_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        file(GLOB _jz_asan_rts "${_jz_cl_dir}/clang_rt.asan*_dynamic-x86_64.dll")
        set(JZ_ASAN_RUNTIME_DLLS "${_jz_asan_rts}" CACHE INTERNAL "MSVC ASan runtime DLLs")
        message(STATUS "ASan runtime: ${_jz_asan_rts}")
    endif()
endif()

#module list
# 注: JZCv(视觉/OCR) **不在这个普通模块列表里** —— 它要求 C++17 且依赖 OpenCV/ONNX Runtime,
#     以**单例模块**形式加入(见文件末尾的 JZ_SINGLE_MODULES)。放进普通模块会把它的源码编进
#     JZFramework(全局 C++14) 里, 既编不过也污染核心库。
set(JZ_MODULE_LIST JZCore JZNet JZAgent JZAudio JZCamera JZCoroutine JZCrashReport
        JZFrp JZGpio JZHttpServer JZJson JZMcpServer JZModbus JZNode
        JZOTA JZSensor JZSerialPort JZTest JZCompress JZXml JZDocument)

# JZWidget(自绘 UI)依赖平台窗口/字体等能力, 目前只在 Windows 下参与构建。
# 关掉它以后 HAS_JZWidget 为假, 各模块里依赖 UI 的源文件会自动跳过(见 JZFramework.cmake)。
if(WIN32)
    list(APPEND JZ_MODULE_LIST JZJSEngine)
    list(APPEND JZ_MODULE_LIST JZWidget)
    # JZGame(2D 游戏控件, 基于 JZWidget): 2026-10-12 首版落地, 按需求**暂不进默认构建**
    # (源码在 JZGame/, 示例在 test/game/; 容量基准见 test/game/bench/sprite_bench)。
    # 需要时取消下一行注释, 并同步打开 JZFramework.cmake 的 MODULE_DEPENDS_JZGame:
    # list(APPEND JZ_MODULE_LIST JZGame)
endif()

include(JZFramework.cmake)

# JZCv(视觉/OCR) 依赖 OpenCV + ONNX Runtime (在上面的 include 里已探测), 缺任一就不建它。
# 它以**单例模块**形式加入: 自己一个 target ⇒ 能单独用 C++17(OCR 内核要求), 不污染核心库。
set(JZ_SINGLE_MODULES "")
if(JZ_OPENCV_FOUND AND JZ_ONNXRUNTIME_FOUND)
    list(APPEND JZ_SINGLE_MODULES JZCv)
endif()

# JZEmulator(GBA 画面控件) 要 JZWidget + 外部的宿主 DLL mgba_host.dll(见 JZEmulator/JZEmulator.cmake)。
# 这里只做"那个 DLL 在不在"的粗判; 精细的探测/接线由 JZEmulator.cmake 与 jz_emulator_enable() 负责。
# 注: 这里只能判 WIN32 —— HAS_JZWidget 要等进 jzframework_use_modules 之后才导出。
set(JZ_MGBA_DIR "E:/work/simulator/host/build" CACHE PATH
    "mgba_host.dll 所在目录(含它的 MinGW 运行库)")
if(WIN32 AND EXISTS "${JZ_MGBA_DIR}/mgba_host.dll")
    list(APPEND JZ_SINGLE_MODULES JZEmulator)
endif()

jzframework_use_modules(${JZ_MODULE_LIST} SINGLE_MODULES ${JZ_SINGLE_MODULES})
# TLS 后端: 用系统 OpenSSL(mongoose 的 MG_TLS_OPENSSL 分支)而不是内置实现。
# 内置实现只支持 P-256 与 RSA —— 证书链里出现 P-384 会直接 "reject secp386 for now"
# 导致握手失败(而且那段链校验不看 skip_verification), 表现为 web_fetch 报 ConnectError。
# OpenSSL 已由 cmake/openssl.cmake 链接到 JZFramework(PUBLIC), 改这一行即可切换。
# ⚠ 影响框架内所有 TLS(HTTPS 服务端 / WSS / MQTTS / TCP+TLS / HTTP 客户端), 改后请跑
#   test_https / test_net_* 回归; 运行期需要 libssl-3-x64.dll / libcrypto-3-x64.dll(已在 PATH)。
target_compile_definitions(JZFramework PRIVATE MG_TLS=MG_TLS_OPENSSL)


option(BUILD_TEST "Build the JZFramework test suite" ON)

# ---------------------------------------------------------------------------
# 所有**非发布 exe**(测试 / 评测 / 开发工具) 的统一输出目录。
#
# 发布 exe 见 binary/CMakeLists.txt 末尾那张"发布清单" —— 它们统一在 <build>/binary/bin/<配置>。
# 于是整个构建树里 exe 只有两个落点:
#   · 发布:  <build>/binary/bin/<配置>
#   · 其余:  <build>/test/bin/<配置>     ← 本变量(名字沿用 test/ 最早那个目录)
#
# 变量在**根目录**定义 ⇒ tools/ test/ binary/ 三个子树都看得到, 谁建"非发布 exe"谁就
# set_target_properties(<t> PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}/$<CONFIG>")。
# ⚠ 但别在 binary/ 那层改用 CMAKE_RUNTIME_OUTPUT_DIRECTORY: 那里发布 exe 与测试 exe 同在一棵
#   子树, 一收就再也分不开(见 binary/CMakeLists.txt 开头的说明)。
# ---------------------------------------------------------------------------
set(JZ_TEST_BIN_DIR "${CMAKE_BINARY_DIR}/test/bin")

# tools 要在 test/binary 之前: 构建期工具(如 jz_uic —— jz_add_ui 生成 C++ 代码用)得先存在,
# 后面那些目录里的 jz_add_ui(...) 才拿得到它的目标(JZFramework.cmake 里会检查)。
add_subdirectory(tools)

if(BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
add_subdirectory(binary)
