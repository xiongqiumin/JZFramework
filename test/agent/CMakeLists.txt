# 需要真实模型/网络的用例: 保持独立(不进 ctest)
add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent JZFramework)

add_executable(test_mcpclient test_mcpclient.cpp)
target_link_libraries(test_mcpclient JZFramework)

# 多模态(图片)用例: 连模型, 独立
add_executable(test_image test_image.cpp)
target_link_libraries(test_image JZFramework)

# ---------------------------------------------------------------------------
# 自包含的工具类用例: 合成一个 test_agent_tools (test_agent_tools <case>)
#
#   code_tools        search_file / search_context(临时工作区, 无网络/窗口/模型)
#   loopdetect        JZToolLoopDetector 纯逻辑
#   terminal_tools    terminal_run / terminal_cancel 工具类(本地 shell)
#   agenttoolbridge   JZMcpToolBridge: 直接喂 JSON-RPC 给 processText(不起窗口/端口)
#
# 每个用例仍是一个进程(ctest 粒度不变), 只链接一次 JZFramework + agent_tool。
# ---------------------------------------------------------------------------
add_executable(test_agent_tools
    main_tools.cpp
    test_code_tools.cpp
    test_loopdetect.cpp
    test_terminal_tools.cpp
    test_agenttoolbridge.cpp)
# JZMcpToolBridge / JZTerminalManager 在 binary/agent_tool 里(通用工具库), 所以要额外链它
target_link_libraries(test_agent_tools JZFramework agent_tool)

# 文件工具类: search_file(按文件名找) / search_context(搜文本·正则) —— 同步回包, 不需要事件循环
# 自包含(临时工作区建在 exe 目录, 无网络 / 无窗口 / 无模型): 进 ctest。
# 这两个工具的回包文案与"文本判定"都是**模型实际看到的行为**, 最容易悄悄回归(踩过: 16KB 采样
# 边界把中文源码判成二进制 ⇒ read_file/modify_file/search_context 一起失效), 必须有自动回归。
add_test(NAME agent.test_code_tools COMMAND test_agent_tools code_tools)
set_tests_properties(agent.test_code_tools PROPERTIES TIMEOUT 120)

add_test(NAME agent.test_loopdetect COMMAND test_agent_tools loopdetect)
set_tests_properties(agent.test_loopdetect PROPERTIES TIMEOUT 60)

add_test(NAME agent.test_terminal_tools COMMAND test_agent_tools terminal_tools)
set_tests_properties(agent.test_terminal_tools PROPERTIES TIMEOUT 120)

add_test(NAME agent.test_agenttoolbridge COMMAND test_agent_tools agenttoolbridge)
set_tests_properties(agent.test_agenttoolbridge PROPERTIES TIMEOUT 120)

# 上下文压缩(TokenCompress): 连真实 LLM(按当前设置 maxToken=150000), 每轮评审同一份
# 固定的 ~1150 行源码(整份进 prompt, 每轮约涨 12k token), 靠 sigLLMHistory 抓"压缩发生"
# 与触发时机; **长跑不退出**(Ctrl+C / Stop-Process 停) —— 不是离线测试, 更不是 ctest 用例,
# 详见源文件头注释
add_executable(test_agent_compress test_agent_compress.cpp)
target_link_libraries(test_agent_compress JZFramework)

add_subdirectory(five_chess)

# FDB-v3（NTU 的语音 agent 工具调用 benchmark）的**文本模式**跑批：
# 去掉 LiveKit/VAD/STT/TTS，只留"输入文本 → agent 工具调用 → 输出文本"，
# 结果落盘成上游 evaluate_*.py 认识的 result_<provider>.json。需要真实模型端点，不进 ctest。
add_subdirectory(fdb_v3)
