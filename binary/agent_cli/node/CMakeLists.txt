# ---------------------------------------------------------------------------
# agent_node —— 装在远程机器上的被控端(源在 agent_cli/node/, 与 cli 同一棵树)
#
# 把本机能力包成一个 MCP server(HTTP / WebSocket)或 link 端点, 本地 code_agent/cli
# 连上来之后, 远端工具就和本地工具混在同一张表里。
#
# 为什么在 agent_cli/ 下: 它和 cli 是同一族的两个角色(node 被连、cli 去连), 协议
# (JZAgentNodePacket)与测试(test/remote_ssh_nodes、hub_nodes)都成对出现, 放一起改
# 一个功能不用跨两棵目录。**目标名仍是 agent_node**(发布清单/test 的 $<TARGET_FILE:agent_node>
# 都不受影响)。
#
# 工具实现全部来自 binary/agent_tool(与 code_agent 共用同一份), 这里只有
# "宿主实现 + 装配 + 命令行"。
# ---------------------------------------------------------------------------
add_executable(agent_node
    main.cpp
    JZAgentNode.h
    JZAgentNode.cpp
)

target_link_libraries(agent_node PRIVATE agent_cli_packet agent_tool)
