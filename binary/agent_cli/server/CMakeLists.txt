# ---------------------------------------------------------------------------
# JZAgentCliServer —— FRP 服务端(frps)
#
# 作为 FRP 中转服务器运行(监听端口等远端 frpc 连入)。与 agent_cli(client 角色 =
# frpc)配对: agent_cli 在私网机把本机 HTTP 端口经 frpc 暴露到这里的 frps。
#
# frps 实现来自 JZFramework 的 JZFrp 模块(JZFrpServer), 链接 JZFramework 即够。
# ---------------------------------------------------------------------------
add_executable(agent_cli_server
    main.cpp
    JZAgentCliServer.h
    JZAgentCliServer.cpp
)

target_link_libraries(agent_cli_server PRIVATE JZFramework)
