# ==================== Dear ImGui (CMake FetchContent 拉取) ====================
# 定位: ImGui 仅作为底层 2D 绘图后端 (ImDrawList + DX11/Win32 backend)。
# JZWidget 保留模式控件树 / 事件系统 / 布局由 JZWidget 模块负责,
# 不使用 ImGui 的即时模式控件 (Begin/Button 等), ImGui 不处理任何交互。
include_guard(GLOBAL)

# ImGui 仅在 Windows 下作为渲染后端 (DX11 + Win32)
if(NOT WIN32)
    message(STATUS "[JZFramework] ImGui backend disabled on non-Windows platform")
    return()
endif()

include(FetchContent)

set(JZ_IMGUI_TAG "v1.92.9b" CACHE STRING "Dear ImGui version tag")

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${JZ_IMGUI_TAG}
    GIT_SHALLOW    TRUE
)

message(STATUS "[JZFramework] Fetching Dear ImGui ${JZ_IMGUI_TAG} ...")
FetchContent_MakeAvailable(imgui)
message(STATUS "[JZFramework] Dear ImGui source dir: ${imgui_SOURCE_DIR}")

add_library(imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    # DX11 绘图后端 + Win32 窗口/消息后端
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
)

target_include_directories(imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

# imgui_impl_dx11 运行时编译着色器需要 d3dcompiler;
# imgui_impl_win32 的 IME 支持需要 imm32。
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler imm32)
