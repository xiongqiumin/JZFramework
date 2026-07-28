# 查找 WebView2 SDK（通过 NuGet 安装的 Microsoft.Web.WebView2 会提供 CMake 配置）
find_package(Microsoft.Web.WebView2 CONFIG QUIET)

# 找不到 SDK：提示并跳过本目标的编译
if (NOT Microsoft.Web.WebView2_FOUND)
    message(STATUS
        "跳过 jzwebview2：未找到 WebView2 SDK。"
        "请安装 NuGet 包 Microsoft.Web.WebView2 后重新配置。")
    return()
endif()

add_executable(jzwebview2
    main.cpp
    JZWebView2.cpp
    JZWebView2Backend.cpp
    JZWebServer.cpp
    JZJSWebApp.cpp)

# COM 系统库 + JZFramework（含 JZHttpServer 后端）
target_link_libraries(jzwebview2 PRIVATE JZFramework ole32.lib uuid.lib advapi32.lib)
target_link_libraries(jzwebview2 PRIVATE Microsoft.Web.WebView2)
