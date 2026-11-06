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
    JZNodeLink.h
    JZNodeLink.cpp
    JZHubLink.h
    JZHubLink.cpp
)

target_link_libraries(agent_cli PRIVATE JZFramework agent_tool)

# 拷贝网页静态资源(http/)到 exe 同目录, 使默认 httpDir(=exeDir/http) 能找到 index.html 等。
add_custom_command(TARGET agent_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/http
        $<TARGET_FILE_DIR:agent_cli>/http
    COMMENT "copy agent_cli web ui (http/)")

# 测试用的大模型接入配置: 测试脚本**不写死**任何端点, 一律读 exe 同目录的 test_llm.json。
# 这里只是"没有就铺一份样例"(见 cmake/copy_if_absent.cmake); 已存在的不覆盖 ——
# 换端点改的是 exe 目录那份, 不是仓库里的模板(test/test_llm.json)。
add_custom_command(TARGET agent_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -Dsrc=${CMAKE_CURRENT_SOURCE_DIR}/test/test_llm.json
        -Ddst=$<TARGET_FILE_DIR:agent_cli>/test_llm.json
        -P ${JZFRAMEWORK_ROOT_DIR}/cmake/copy_if_absent.cmake
    COMMENT "Place test_llm.json next to agent_cli (sample if absent)")

# test: --loop 端到端等测试(见 test/CMakeLists.txt; 测试脚本与它的验证程序都放在那里)
add_subdirectory(test)

# agent_cli_test: 复用 code_agent/benchmark 用例评测 JZAgentCli(见 agent_cli_test/CMakeLists.txt)
add_subdirectory(agent_cli_test)

# JZAgentCliServer: FRP 服务端(frps) —— 与 agent_cli(client 角色 = frpc)配对
add_subdirectory(server)
