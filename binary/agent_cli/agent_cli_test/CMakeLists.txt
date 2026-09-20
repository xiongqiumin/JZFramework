# agent_cli_test —— 用 code_agent/benchmark 的同一套用例评测 JZAgentCli
#
# 与 code_agent_benchmark 的区别: 这里不跑 JZCodeAgent + code_runtime, 而是把用例直接喂给
# JZAgentCli(它自己就跑一个 JZAgent + 本地 agent_tool 工具)的 submitTask, 走 agent_cli
# 原生的"提交 -> 验证脚本门控"循环; 循环结束的 JSON 结果(status/verify_rounds/...)即判定依据。
#
# 用例加载逻辑与 benchmark 完全一致(同 parseCaseObject/loadCases), 因此 code_agent/benchmark/data
# 里的用例可直接作为本测试的"输入", 无需改写:
#   - verify 字段(外部验证命令模板) -> submitTask 的 verify_cmd(JZAgentCli 原生验证机制);
#   - 无 verify 的用例(expect.contains 型) -> 不挂验证, status 以 agent 自身成败为准(烟雾测试)。
# 注: benchmark 的 verify 命令通常假定由 code_runtime 跑产物(server 型用例), 在 agent_cli 下由
#      agent 自行用工具驱动, 语义不完全一致, 此类用例可能 FAIL —— 这是数据层面的差异, 非测试 bug。

set(AGENT_CLI_TEST_SOURCES
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZAgentCli.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZAgentCli.h
)

add_executable(agent_cli_test ${AGENT_CLI_TEST_SOURCES})
target_include_directories(agent_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(agent_cli_test PRIVATE JZFramework agent_tool)

# 复用 code_agent/benchmark 的评测集(data/*.json), 随构建拷贝到 exe 同目录的 data/agent_cli_test
# （运行期由 applicationDirPath()+"/data/agent_cli_test" 读取；可用 --data 覆盖）。
add_custom_command(TARGET agent_cli_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../code_agent/benchmark/data"
            "$<TARGET_FILE_DIR:agent_cli_test>/data/agent_cli_test"
    COMMENT "Copy benchmark evaluation set into bin/<CONFIG>/data/agent_cli_test")
