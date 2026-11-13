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
	JZAgentCliTask.h
	JZAgentCliTask.cpp
    JZNodeLink.h
    JZNodeLink.cpp
    JZHubLink.h
    JZHubLink.cpp
)

# ---------------------------------------------------------------------------
# agent_cli_packet —— 这一族的**线上协议**(执行面 + 控制面)共用静态库
#
# 为什么是静态库而不是放进 JZAgent 模块(以前就放在那儿):
#   * 这两个协议只有 agent_cli / agent_node / agent_cli_server / agent_cli_test 用,
#     框架(JZFramework.dll)里没有任何代码引用它们 ⇒ 放进去等于白占导出面;
#   * 静态库直接编进各自 exe, 互不共享状态, 也就不需要 JZFRAMEWORK_API(见头文件里的说明)。
# 内容: 包类(JZNetPack 派生) + 两个幂等注册函数 JZAgentNodePacketInit/JZAgentHubPacketInit。
# ---------------------------------------------------------------------------
add_library(agent_cli_packet STATIC
    JZAgentHubPacket.h
    JZAgentHubPacket.cpp
    JZAgentNodePacket.h
    JZAgentNodePacket.cpp
)
target_link_libraries(agent_cli_packet PUBLIC JZFramework)
# PUBLIC: server/ node/ test/ 这几个子目录里的源文件都按裸名 include 这两个头。
target_include_directories(agent_cli_packet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(agent_cli PRIVATE agent_cli_packet agent_tool)

# 拷贝网页静态资源(webui/)到 exe 同目录, 使默认 httpDir(=exeDir/webui) 能找到 index.html 等。
# 目录就叫 webui: 它是"网页界面", 不是 http 服务本身(别和 http 配置段混在一起)。
add_custom_command(TARGET agent_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/webui
        $<TARGET_FILE_DIR:agent_cli>/webui
    COMMENT "copy agent_cli web ui (webui/)")

# 统一视觉层(web/ui: tokens/base/components + demo) —— 一份源, 每个 web 宿主各拷一份。
# 页面里用 /ui/*.css 引用(静态目录根就是 webui/)。
add_custom_command(TARGET agent_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${JZFRAMEWORK_ROOT_DIR}/web/ui
        $<TARGET_FILE_DIR:agent_cli>/webui/ui
    COMMENT "copy shared web ui (web/ui -> webui/ui)")

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
#   评测程序 agent_cli_test(复用 benchmark/ 用例评测 JZAgentCli)也在 test/ 下面,
#   由 test/CMakeLists.txt 自己 add_subdirectory —— 与其它测试脚本同一处管理。
add_subdirectory(test)

# JZAgentCliServer: FRP 服务端(frps) —— 与 agent_cli(client 角色 = frpc)配对
add_subdirectory(server)

# node/: 远端被控端(原 binary/agent_node) —— 与 cli 配对的另一半, 放在同一棵树里
add_subdirectory(node)
