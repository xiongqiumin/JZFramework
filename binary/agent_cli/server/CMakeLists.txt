# ---------------------------------------------------------------------------
# JZAgentCliServer —— agent_cli 管理服务器
#   控制面(hub) + 后台网页/管理 API + 本机 agent_cli 子进程监管。
#   内网穿透由独立的 jzfrps / jzfrpc 进程负责, 不在本 target 内。
# ---------------------------------------------------------------------------
add_executable(agent_cli_server
    main.cpp
    JZAgentCliServer.h
    JZAgentCliServer.cpp
    JZServerAgent.h
    JZServerAgent.cpp
)

# agent_tool: server 内置 agent 复用同一份终端/文件工具(与 agent_cli 一致)
target_link_libraries(agent_cli_server PRIVATE agent_cli_packet agent_tool)

# 管理台网页: web/(index.html + app.css + app.js) 拷到 <exeDir>/server_web/,
# 再把仓库唯一的视觉层 web/ui 拷进 server_web/ui —— 页面不再内嵌在 C++ 字符串里。
# ⚠ 单独用 server_web 这个目录: agent_cli 自己也往 <exeDir>/http 拷 index.html, 不能撞。
add_custom_command(TARGET agent_cli_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/web
        $<TARGET_FILE_DIR:agent_cli_server>/server_web
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${JZFRAMEWORK_ROOT_DIR}/web/ui
        $<TARGET_FILE_DIR:agent_cli_server>/server_web/ui
    COMMENT "copy server web (server_web/ + shared ui/)")
