# code_agent_benchmark - 用例评测（端到端：大模型生成代码 -> 宿主 tsc 编译
#   -> code_runtime 隔离运行 -> 校验输出）。headless、无 GUI、需要大模型。
#
# 每个用例：用临时目录初始化标准 TS 工程 -> 把任务发给 JZCodeAgent 自驱完成 ->
#   宿主真实 tsc 打包/发布 -> 调 code_runtime 子进程跑产物 -> 按用例 check() 判定；
#   不符则把实际输出反馈给模型重试，最多 maxAttempts 次。
#
# 编译只需 code_agent 中与 GUI 无关的源文件（MainWindow/ChatView 不参与）。
# 运行需 code_runtime 子进程，保证其先构建。

set(CODE_AGENT_BENCH_SOURCES
    main.cpp
)

add_executable(code_agent_benchmark ${CODE_AGENT_BENCH_SOURCES})
target_include_directories(code_agent_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(code_agent_benchmark PRIVATE JZFramework)
target_link_libraries(code_agent_benchmark PRIVATE code_agent_core)

# 评测要"调用运行时"（code_runtime 子进程），保证其先构建
add_dependencies(code_agent_benchmark code_runtime)