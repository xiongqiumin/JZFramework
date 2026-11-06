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
#   test_llm.json 模板, 构建时按"缺失才铺"拷到 exe 目录); 也可用同名命令行参数
#   (--model-url/--model-name/--model-token) 或环境变量 JZ_BENCH_MODEL_* 覆盖。
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

#   test_codex/test_codex_bench.py —— 用 **Codex SDK** 跑 benchmark 的同一批用例(与 agent_cli_test
#       同套用例/工程骨架/验证命令), 用来和 agent_cli 的通过率做对比。真模型 + 真 Codex 回合 ⇒ 很慢,
#       同样挂 llm 标签(不混进快速回归):
#           ctest -C Release -R agent_cli.codex_bench --output-on-failure
#       想先看要跑什么(不连模型): python test_codex/test_codex_bench.py --dry-run
#       ⚠ Codex 每轮发 role="developer", 本机 llama.cpp 只认 system 在开头 ⇒ 必须经
#         test_codex/codex_proxy.py 适配(developer -> user), 细节见那个文件与 bench 脚本的头注释。
#       没装 openai-codex SDK 时返回 77 ⇒ 跳过(不算失败)。
add_test(NAME agent_cli.codex_bench
         COMMAND ${JZ_PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_codex/test_codex_bench.py)
set_tests_properties(agent_cli.codex_bench PROPERTIES TIMEOUT 14400 LABELS "llm" RUN_SERIAL TRUE
                     SKIP_RETURN_CODE 77)
