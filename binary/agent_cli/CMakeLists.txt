# ---------------------------------------------------------------------------
# agent_cli —— 通过 HTTP 暴露任务提交的 agent
#
# 自己跑一个 JZAgent(直接驱动本地 agent_tool 工具), 通过 HTTP(POST /task + 内置
# 聊天式网页控制台 GET /) 对外暴露。与 agent_node 的区别见 JZAgentCli.h / main.cpp
# 头部注释。
#
# 工具实现全部来自 binary/agent_tool(与本地 code_agent / 远端 agent_node 共用同一份),
# JZAgent 来自 JZFramework。
# ---------------------------------------------------------------------------
add_executable(agent_cli
    main.cpp
    JZAgentCli.h
    JZAgentCli.cpp
)

target_link_libraries(agent_cli PRIVATE JZFramework agent_tool)

# 拷贝网页静态资源(http/)到 exe 同目录, 使默认 httpDir(=exeDir/http) 能找到 index.html 等。
add_custom_command(TARGET agent_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/http
        $<TARGET_FILE_DIR:agent_cli>/http
    COMMENT "copy agent_cli web ui (http/)")

# test: --loop 端到端等测试(见 test/CMakeLists.txt; 测试脚本与它的验证程序都放在那里)
add_subdirectory(test)

# agent_cli_test: 复用 code_agent/benchmark 用例评测 JZAgentCli(见 agent_cli_test/CMakeLists.txt)
add_subdirectory(agent_cli_test)

# JZAgentCliServer: FRP 服务端(frps) —— 与 agent_cli(client 角色 = frpc)配对
add_subdirectory(server)
