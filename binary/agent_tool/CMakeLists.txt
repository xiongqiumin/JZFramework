# ---------------------------------------------------------------------------
# agent_tool —— 通用 agent 工具库
#
# 把"模型能对一台机器做的事"和"谁来托管这些工具"切开:
#   - 本库只有工具实现与策略(区域模型 / 命令白名单 / 虚拟终端 / 文件编辑),
#     不认识任何宿主;
#   - 需要宿主提供的东西(工作区、读文件、打包)走 ICodeToolHost, 由宿主实现;
#   - 本地 code_agent 用 JZAgentTool 做宿主适配, 远端 agent_node 用 JZMcpTool
#     桥接同一批工具 —— 实现只有一份, 权限规则也只有一份。
#
# 约束: 只依赖 JZFramework。**不许**依赖 binary/code_agent 里的任何东西
# (那会形成反向依赖, 也会把 JZWidget/MainWindow 拖进 node)。
# ---------------------------------------------------------------------------
add_library(agent_tool STATIC
    JZCodeTools.h
    JZCodeTools.cpp
    JZShellParser.h
    JZShellParser.cpp
    JZVirtualTerminalConfig.h
    JZVirtualTerminalConfig.cpp
    JZMcpToolBridge.h
    JZMcpToolBridge.cpp
    JZVirtualTerminal.h
    JZVirtualTerminal.cpp
    JZTerminalTools.h
    JZTerminalTools.cpp
    JZTerminalMcpTools.h
    JZTerminalMcpTools.cpp
)

target_link_libraries(agent_tool PUBLIC JZFramework)
target_include_directories(agent_tool PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
