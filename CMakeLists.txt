cmake_minimum_required(VERSION 3.10)
project(JZFramework C CXX ASM)

set(CMAKE_CXX_STANDARD 14)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
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
set(JZ_MODULE_LIST JZCore JZNet JZAgent JZAudio JZCamera JZCoroutine JZCrashReport
        JZCv JZFrp JZGpio JZHttpServer JZJSEngine JZJson JZMcpServer JZModbus JZNode
        JZOTA JZSensor JZSerialPort JZTest JZCompress JZXml)

# JZWidget(自绘 UI)依赖平台窗口/字体等能力, 目前只在 Windows 下参与构建。
# 关掉它以后 HAS_JZWidget 为假, 各模块里依赖 UI 的源文件会自动跳过(见 JZFramework.cmake)。
if(WIN32)
    list(APPEND JZ_MODULE_LIST JZWidget)
endif()

include(JZFramework.cmake)
jzframework_use_modules(${JZ_MODULE_LIST})
target_compile_definitions(JZFramework PRIVATE MG_TLS=MG_TLS_BUILTIN)

option(BUILD_TEST "Build the JZFramework test suite" ON)

add_subdirectory(demo)
if(BUILD_TEST)
    enable_testing()
    add_subdirectory(test)
endif()
add_subdirectory(tools)
add_subdirectory(binary)
