# ---------------------------------------------------------------------------
# desktop —— agent_cli 的桌面壳(当前是 CEF 占位)
#
# 2026-11-13 合并收尾: 原 code_agent 的自绘 UI、JZTs* 三个工具、以及 JZCodeAgent
#   那套重复实现的 agent 循环/工具表/提示词全部删除; 评测统一到
#   binary/agent_cli/test/agent_cli_test, 所以连 code_agent_core 都不需要了。
#   desktop 现在只编译一个浏览器壳占位(见 JZCefHost.h)。
#
# 本目录剩下的非 UI 文件(JZExeResource / JZSourceMap / JZScreenShot)
# 不是 desktop 的一部分, 只是运行时共用源码 —— 由 binary/js_runtime /
# binary/virtual_device / test_* 直接编译(见各自 CMakeLists)。
# ---------------------------------------------------------------------------
add_executable(code_agent
    main.cpp
    JZCefHost.h
    JZCefHost.cpp
)
target_link_libraries(code_agent PRIVATE JZFramework)

jz_copy_asan_runtime(code_agent)
