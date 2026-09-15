# ---------------------------------------------------------------------------
# agent_node —— 装在远程机器上的被控端
#
# 把本机能力包成一个 MCP server(HTTP / WebSocket), 本地 code_agent 用 JZMcpClient
# 连上来之后, 远端工具就和本地工具混在同一张表里。
#
# 工具实现全部来自 binary/agent_tool(与本地 code_agent 共用同一份), 这里只有
# "宿主实现 + 装配 + 命令行"。
# ---------------------------------------------------------------------------
add_executable(agent_node
    main.cpp
    JZAgentNode.h
    JZAgentNode.cpp
)

target_link_libraries(agent_node PRIVATE JZFramework agent_tool)
