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
#   模型走 JZ_BENCH_MODEL_URL / JZ_BENCH_MODEL_NAME / JZ_BENCH_MODEL_TOKEN(或同名命令行参数)。
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
