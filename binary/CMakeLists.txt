add_subdirectory(agent_tool)
add_subdirectory(agent_node)
add_subdirectory(agent_cli)
add_subdirectory(frpc)

# code_agent 是自绘 UI 程序, 依赖 JZWidget(见顶层 CMakeLists: 目前仅 Windows 启用)
if(HAS_JZWidget)
add_subdirectory(updater)
# virtual_device: 虚拟桌面(虚拟显示器 + 窗口管理器) + 它的宿主程序。
# 同时提供静态库 jz_virtual_device 给 CU / 测试链 (见 binary/virtual_device/CMakeLists.txt)。
add_subdirectory(virtual_device)
add_subdirectory(code_agent)
endif()
