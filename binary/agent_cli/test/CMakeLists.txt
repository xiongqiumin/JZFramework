# ---------------------------------------------------------------------------
# agent_cli 的测试(与 binary/code_agent/test 同套路: 测试放各自的 test/ 子目录, 自己注册 ctest)
#
#   test_loop_primes.py —— 无人值守端到端(需要真实模型 + 本机 python, 跑几分钟):
#       agent_loop.py 拉起 agent_cli(HTTP) 并逐轮 POST /task; 验证程序门控 + 每轮验证通过后
#       备份(只复制, 不碰 git) + [loop-stop] 收工 + 收工后的外部复验。
#       用例是质数筛: N 每轮随机取, 参考值由验证程序现场算 ⇒ 判定就是"两个整数相等",
#       比"算圆周率再比字符串前缀"那种判定硬, 也没法把答案写死。
#       因为慢且依赖本地模型, 单独挂 llm 标签(不混进快速回归):
#           ctest -C Release -R agent_cli.loop_primes --output-on-failure
#   模型配置**不在代码/脚本里写死**: 读 agent_cli.exe 同目录的 test_llm.json(见同目录的
#   test_llm.json 模板, 构建时按"缺失才铺"拷到 exe 目录); 临时换端点用同名命令行参数
#   (--model-url/--model-name/--model-token) 覆盖。
#
#   primes_verify.py —— 上面那个用例的验证程序(也是"验证程序该长什么样"的模板):
#       由 test_loop_primes.py / agent_loop.py 作为 verify_cmd 传进去, 不作为独立 ctest 条目。
#
#   test_icon_find.py —— 图标查找回归(先用 OpenCV 后端, VLM/YOLO 后端留接口):
#       合成截图场景 -> build 出图标库模型 -> find 定位 -> 断言召回/类别/中心点误差/零误检。
#       纯本机计算(不需要模型, 不需要 GPU), 秒级, 直接进快速回归:
#           ctest -C Release -R agent_cli.icon_find --output-on-failure
#       缺 numpy/opencv 时返回 77 ⇒ 跳过(不算失败)。
# ---------------------------------------------------------------------------
add_test(NAME agent_cli.loop_primes
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_loop_primes.py
                 --agent-cli $<TARGET_FILE:agent_cli>)
set_tests_properties(agent_cli.loop_primes PROPERTIES TIMEOUT 3600 LABELS "llm" RUN_SERIAL TRUE)

add_test(NAME agent_cli.icon_find
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_icon_find.py)
set_tests_properties(agent_cli.icon_find PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

#   test_remote_ssh_nodes.py —— 2 个 link 模式 agent_node + 1 个 agent_cli:
#       模型只看到一个 remote_ssh 工具, 用它把 nodeA 的 secret.txt 搬到 nodeB 的 answer.txt。
#       默认用内置的 **mock 模型**(OpenAI 兼容 + SSE, 固定回两次 tool_call + 一次收尾),
#       所以不联网、秒级、可进快速回归; 断言是"逐字符相等"(内容随机), 且验证命令只读
#       nodeB 的文件 ⇒ 必须真的经 remote_ssh 搬运。要验真模型: 加 --real-llm(慢)。
#           ctest -C Release -R agent_cli.remote_ssh_nodes --output-on-failure
add_test(NAME agent_cli.remote_ssh_nodes
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_remote_ssh_nodes.py
                 --agent-cli $<TARGET_FILE:agent_cli>
                 --agent-node $<TARGET_FILE:agent_node>)
set_tests_properties(agent_cli.remote_ssh_nodes PROPERTIES
                     TIMEOUT 300 LABELS "agent_cli" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

#   test_hub_nodes.py —— server -> cli(managed) -> node 全链路:
#       任务从 agent_cli_server 的 POST /api/tasks 提交, 经控制面(JZNet)下发给 cli,
#       cli 再用 remote_ssh 把 nodeA 的 secret.txt 搬到 nodeB 的 answer.txt。
#       顺带断言 managed 模式**不监听** http.port(与 standalone 互斥)。
#           ctest -C Release -R agent_cli.hub_nodes --output-on-failure
add_test(NAME agent_cli.hub_nodes
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_hub_nodes.py
                 --agent-cli $<TARGET_FILE:agent_cli>
                 --agent-node $<TARGET_FILE:agent_node>
                 --agent-server $<TARGET_FILE:agent_cli_server>)
set_tests_properties(agent_cli.hub_nodes PROPERTIES
                     TIMEOUT 300 LABELS "agent_cli" RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

#   test_task_queues.py —— 多任务队列(task.queues): 每个队列一个独立 JZAgent,
#       队列内串行、队列间并发、全忙排队。mock 模型每个请求睡 1.5s, 用耗时区分并发/串行。
#           ctest -C Release -R agent_cli.task_queues --output-on-failure
add_test(NAME agent_cli.task_queues
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_task_queues.py
                 --agent-cli $<TARGET_FILE:agent_cli>)
set_tests_properties(agent_cli.task_queues PROPERTIES
                     TIMEOUT 180 LABELS "agent_cli" RUN_SERIAL TRUE)

# 队列取消/多队列事件历史 + managed 异步任务生命周期(均用本机 mock, 快速回归)
add_test(NAME agent_cli.task_cancel
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_task_cancel.py
                 --agent-cli $<TARGET_FILE:agent_cli>
                 --agent-node $<TARGET_FILE:agent_node>)
set_tests_properties(agent_cli.task_cancel PROPERTIES
                     TIMEOUT 120 LABELS "agent_cli" RUN_SERIAL TRUE)

add_test(NAME agent_cli.server_tasks
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_server_tasks.py
                 --agent-cli $<TARGET_FILE:agent_cli>
                 --agent-server $<TARGET_FILE:agent_cli_server>)
set_tests_properties(agent_cli.server_tasks PROPERTIES
                     TIMEOUT 120 LABELS "agent_cli" RUN_SERIAL TRUE)

# ---------------------------------------------------------------------------
# agent_cli_test.cpp —— 评测程序(不是 ctest 条目): 用 code_agent/benchmark 的同一套用例
#   评测 JZAgentCli 本体(submitTask + 验证脚本门控); 与 code_agent_benchmark 的区别
#   见文件头注释。
#       cd build/test/bin/<配置>
#       agent_cli_test.exe --cases 'hello_*' --no-color
#   输出到 <build>/test/bin/<配置>(不发布), 用例集与 test_llm.json 由下面 POST_BUILD 铺到旁边。
# ---------------------------------------------------------------------------
add_executable(agent_cli_test
    agent_cli_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZAgentCli.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZAgentCli.h
    # JZAgentCli 依赖的传输层: node(执行面)与 server(控制面)
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZNodeLink.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../JZHubLink.cpp
)
target_include_directories(agent_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
# agent_cli_packet: 两个协议包的**定义**(JZAgentNode/HubPacketInit)在静态库里, 见 ../CMakeLists.txt
target_link_libraries(agent_cli_test PRIVATE agent_cli_packet agent_tool)

set_target_properties(agent_cli_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}/$<CONFIG>")

# 复用 code_agent/benchmark 的评测集(data/*.json), 随构建拷贝到 exe 同目录的 data/agent_cli_test
# （运行期由 applicationDirPath()+"/data/agent_cli_test" 读取；可用 --data 覆盖）。
add_custom_command(TARGET agent_cli_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/../../code_agent/benchmark/data"
            "$<TARGET_FILE_DIR:agent_cli_test>/data/agent_cli_test"
    COMMENT "Copy benchmark evaluation set next to agent_cli_test (data/agent_cli_test)")

# 大模型接入配置: 测试**不写死**任何端点, 一律读 exe 同目录的 test_llm.json(模板就在本目录)。
# 这里只是"没有就铺一份样例"(见 cmake/copy_if_absent.cmake) —— 已经存在的**不覆盖**,
# 因为那份才是本机实际在用的(换端点改的是它, 不是仓库里这份模板)。
add_custom_command(TARGET agent_cli_test POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -Dsrc=${CMAKE_CURRENT_SOURCE_DIR}/test_llm.json
            -Ddst=$<TARGET_FILE_DIR:agent_cli_test>/test_llm.json
            -P ${JZFRAMEWORK_ROOT_DIR}/cmake/copy_if_absent.cmake
    COMMENT "Place test_llm.json next to agent_cli_test (sample if absent)")

#   test_codex/test_codex_bench.py —— 用 **Codex SDK** 跑 benchmark 的同一批用例(与 agent_cli_test
#       同套用例/工程骨架/验证命令), 用来和 agent_cli 的通过率做对比。
#       ⚠ 真模型 + 真 Codex 回合, 全量一轮动辄几十分钟, **不再注册 ctest**(2026-11-09 移除),
#         只手动跑:
#           python binary/agent_cli/test/test_codex/test_codex_bench.py
#           python binary/agent_cli/test/test_codex/test_codex_bench.py --dry-run   # 只看要跑什么
#       ⚠ Codex 每轮发 role="developer", 本机 llama.cpp 只认 system 在开头 ⇒ 必须经
#         test_codex/codex_proxy.py 适配(developer -> user), 细节见那个文件与 bench 脚本的头注释。
