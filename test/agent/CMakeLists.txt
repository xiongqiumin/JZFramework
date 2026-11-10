add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent JZFramework)

add_executable(test_mcpclient test_mcpclient.cpp)
target_link_libraries(test_mcpclient JZFramework)

add_executable(test_image test_image.cpp)
target_link_libraries(test_image JZFramework)

# JZMcpToolBridge / JZTerminalManager 在 binary/agent_tool 里(通用工具库), 所以要额外链它
add_executable(test_agenttoolbridge test_agenttoolbridge.cpp)
target_link_libraries(test_agenttoolbridge JZFramework agent_tool)

# test_virtual_terminal 已删除(2026-11-10): 它是旧解释器时代的产物, 断言前提已不成立
#   (最典型的一条: "单独一条 export 必须被拒, 因为变量不跨命令保留" —— 现在终端复用同一个
#    JZShell, export 确实跨命令保留)。而且它的步骤链是"回调里直接调下一步", 没有重入保护:
#   一旦某步的 run 卡住, 回调被反复触发 ⇒ 步骤按 2^n 递归, 一次运行能刷出 20 万条失败。
#   终端行为的回归改由 ctest 里的 agent.test_code_tools / test_terminal_tools 覆盖。

# terminal_run / terminal_query / terminal_cancel 三个工具类(终端 API 见上一条)
add_executable(test_terminal_tools test_terminal_tools.cpp)
target_link_libraries(test_terminal_tools JZFramework agent_tool)

# 工具循环检测(纯逻辑, 不需要模型/网络/事件循环)
add_executable(test_loopdetect test_loopdetect.cpp)
target_link_libraries(test_loopdetect JZFramework)

# 文件工具类: search_file(按文件名找) / search_context(搜文本·正则) —— 同步回包, 不需要事件循环
add_executable(test_code_tools test_code_tools.cpp)
target_link_libraries(test_code_tools JZFramework agent_tool)
# 自包含(临时工作区建在 exe 目录, 无网络 / 无窗口 / 无模型): 进 ctest。
# 这两个工具的回包文案与"文本判定"都是**模型实际看到的行为**, 最容易悄悄回归(踩过: 16KB 采样
# 边界把中文源码判成二进制 ⇒ read_file/modify_file/search_context 一起失效), 必须有自动回归。
add_test(NAME agent.test_code_tools COMMAND test_code_tools)
set_tests_properties(agent.test_code_tools PROPERTIES TIMEOUT 120)

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
