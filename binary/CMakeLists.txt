add_subdirectory(agent_tool)
add_subdirectory(agent_node)
add_subdirectory(frpc)
add_subdirectory(updater)

# code_agent 是自绘 UI 程序, 依赖 JZWidget(见顶层 CMakeLists: 目前仅 Windows 启用)
if(HAS_JZWidget)
add_subdirectory(code_agent)
endif()
