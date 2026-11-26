# ---------------------------------------------------------------------------
# desktop —— agent_cli 的桌面壳(第一版: COM 直连 WebView2)
#
# 2026-11-25: 原 JZCefHost 占位换成 JZWebView2Host —— Win32 窗口 + WebView2 COM,
# 默认加载 exe 同目录的 test_page.html; 命令行参数可换成 agent_cli 的 HTTP 地址。
#
# WebView2 native SDK 从本机 NuGet 缓存自动找; 可用 -DJZ_WEBVIEW2_SDK_DIR=<dir>
# 覆盖(该目录需含 include/WebView2.h 与 x64|/x86/WebView2Loader.dll.lib)。
# 没找到就跳过 code_agent, 不影响其余构建。
#
# 本目录剩下的非 UI 文件(JZExeResource / JZSourceMap / JZScreenShot)
# 不是 desktop 的一部分, 只是运行时共用源码 —— 由 binary/js_runtime /
# binary/virtual_device / test_* 直接编译(见各自 CMakeLists)。
# ---------------------------------------------------------------------------
set(JZ_WEBVIEW2_SDK_DIR "" CACHE PATH
    "WebView2 native SDK root (dir with include/ and x64| x86/)")

if(NOT JZ_WEBVIEW2_SDK_DIR)
    file(GLOB _jz_wv2_native LIST_DIRECTORIES true
        "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/*/build/native")
    if(_jz_wv2_native)
        list(SORT _jz_wv2_native)
        list(LENGTH _jz_wv2_native _jz_wv2_count)
        math(EXPR _jz_wv2_last "${_jz_wv2_count} - 1")
        list(GET _jz_wv2_native ${_jz_wv2_last} _jz_wv2_pick)
        set(JZ_WEBVIEW2_SDK_DIR "${_jz_wv2_pick}" CACHE PATH
            "WebView2 native SDK root (dir with include/ and x64| x86/)" FORCE)
    endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_jz_wv2_arch x64)
else()
    set(_jz_wv2_arch x86)
endif()
set(_jz_wv2_include "${JZ_WEBVIEW2_SDK_DIR}/include")
set(_jz_wv2_lib "${JZ_WEBVIEW2_SDK_DIR}/${_jz_wv2_arch}/WebView2Loader.dll.lib")
set(_jz_wv2_dll "${JZ_WEBVIEW2_SDK_DIR}/${_jz_wv2_arch}/WebView2Loader.dll")

if(EXISTS "${_jz_wv2_include}/WebView2.h" AND EXISTS "${_jz_wv2_lib}")
    add_executable(code_agent
        main.cpp
        JZWebView2Host.h
        JZWebView2Host.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZAgentCliServer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZAgentCliServer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZAgentTaskStore.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZAgentTaskStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZInprocCli.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZInprocCli.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZServerExtraTools.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../server/JZServerExtraTools.cpp
    )
    target_include_directories(code_agent PRIVATE
        "${_jz_wv2_include}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../server"
    )
    target_link_libraries(code_agent PRIVATE JZFramework agent_cli_core "${_jz_wv2_lib}" ole32 user32 ws2_32)

    add_custom_command(TARGET code_agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_jz_wv2_dll}" "$<TARGET_FILE_DIR:code_agent>/WebView2Loader.dll"
        COMMENT "Copy WebView2Loader.dll next to code_agent")
    add_custom_command(TARGET code_agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/test_page.html"
            "$<TARGET_FILE_DIR:code_agent>/test_page.html"
        COMMENT "Copy WebView2 smoke test page")
    add_custom_command(TARGET code_agent POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../server/webui"
            "$<TARGET_FILE_DIR:code_agent>/server_web"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZFRAMEWORK_ROOT_DIR}/web/ui"
            "$<TARGET_FILE_DIR:code_agent>/server_web/ui"
        COMMENT "Copy server web for desktop shell")

    jz_copy_asan_runtime(code_agent)
else()
    message(STATUS
        "desktop: WebView2 native SDK not found (JZ_WEBVIEW2_SDK_DIR='${JZ_WEBVIEW2_SDK_DIR}'); code_agent skipped")
endif()
