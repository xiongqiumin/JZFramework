add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent JZFramework)

add_executable(test_mcpclient test_mcpclient.cpp)
target_link_libraries(test_mcpclient JZFramework)

add_executable(test_image test_image.cpp)
target_link_libraries(test_image JZFramework)

# JZMcpToolBridge / JZTerminalManager 在 binary/agent_tool 里(通用工具库), 所以要额外链它
add_executable(test_agenttoolbridge test_agenttoolbridge.cpp)
target_link_libraries(test_agenttoolbridge JZFramework agent_tool)

add_executable(test_virtual_terminal test_virtual_terminal.cpp)
target_link_libraries(test_virtual_terminal JZFramework agent_tool)

# 工具循环检测(纯逻辑, 不需要模型/网络/事件循环)
add_executable(test_loopdetect test_loopdetect.cpp)
target_link_libraries(test_loopdetect JZFramework)

add_subdirectory(five_chess)
