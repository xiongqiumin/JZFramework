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

# 输出目录：本目录的 exe 是**评测**用的，与其它"非发布 exe"统一到 <build>/test/bin/<配置>
# (根 CMakeLists 的 JZ_TEST_BIN_DIR)，不进发布目录(<build>/binary/bin/<配置>/,
# 见 binary/CMakeLists.txt 的那张"发布清单")。
# ⚠ 它要的 code_runtime / ts_compiler 在发布目录里 ⇒ main.cpp 走 JZBundlePaths.h 沿上级目录
#   把发布目录找回来(<build>/test/bin/<配置> 往上同样能命中 <build>/binary/bin/<配置>)；
#   评测集 data/ 则由下面的 POST_BUILD 拷到 exe 旁边($<TARGET_FILE_DIR>)，
#   所以随手改 exe 的输出目录都不会散架。
set_target_properties(code_agent_benchmark PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}/$<CONFIG>")

# 评测集（data/*.json）随构建拷贝到 exe 同目录（bin/data），运行期由
# applicationDirPath()+"/data" 读取。
# 用 $<TARGET_FILE_DIR:...> 而非硬编码 bin 路径：它解析为该目标真实的输出目录
# （父 CMake 现已按配置分目录 bin/<CONFIG>，Debug/Release 各自独立），
# 自动对配置正确——data 始终落到当前配置 exe 所在目录下的 data/benchmark
# （如 bin/Debug/data/benchmark）。
add_custom_command(TARGET code_agent_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:code_agent_benchmark>/data/benchmark"
    COMMENT "Copy benchmark evaluation set (data/*.json) into bin/<CONFIG>/data/benchmark")