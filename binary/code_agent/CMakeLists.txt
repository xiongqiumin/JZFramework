# binary/code_agent: 基于 JZAgent + JZHttpServer 的本地代码助手（网页版）
#
# 依赖模块: JZAgent(大模型/工具调用) JZHttpServer(网页服务) JZWebView(内嵌窗口, libcef)
# 依赖由 JZFramework.cmake 自动解析。
# 仅 Windows 下编译（由 binary/CMakeLists.txt 控制，非 WIN32 不 add_subdirectory）。

set(CODE_AGENT_SOURCES
    main.cpp
    JZCodeTools.h
    JZCodeTools.cpp
    JZCodeAgent.h
    JZCodeAgent.cpp
    JZCodeAgentServer.h
    JZCodeAgentServer.cpp
    JZSpeechClient.h
    JZSpeechClient.cpp
)

# 内嵌网页资源: web/code_agent.qrc -> 静态库 code_agent_web (暴露 jzres_code_agent_init())
jz_add_resources(code_agent_web ${CMAKE_CURRENT_LIST_DIR}/web/code_agent.qrc)

add_executable(code_agent ${CODE_AGENT_SOURCES})

target_link_libraries(code_agent PRIVATE
    JZFramework
    code_agent_web
)