# ---------------------------------------------------------------------------
# 测试 exe 的输出目录：统一到 <build>/test/bin/<CONFIG>（= 根 CMakeLists 的 JZ_TEST_BIN_DIR，
# 那里的"非发布 exe"都往这儿放：binary/code_agent/test、code_agent/benchmark、agent_cli_test、
# tools/jz_uic、tools/monitor 也是同一个目录）。
#
# 为什么这里可以(而 binary/ 里不行)：本目录以下**建的全是测试 exe**，没有"发布"要区分,
# 所以用 CMAKE_RUNTIME_OUTPUT_DIRECTORY 一把收干净最省事, 也不必逐个列 target 名
# (test/ 下的 target 会随测试增删, 写死一张表迟早漏)。
# ⚠ binary/CMakeLists.txt 里禁的是"在 binary/ 那一层"用它 —— 那里发布 exe 与测试 exe 混在一棵子树,
#   一收就再也分不开。两处口径不同, 别互相"统一"。
# 注: 多配置生成器(VS)会自动再按配置分子目录 = <build>/test/bin/Debug|Release。
#     改这里只影响"新的构建", 旧位置(<build>/test/<子目录>/<配置>/)的 exe 会变成历史残留, 可删。
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}")

add_subdirectory(net)
add_subdirectory(co)
add_subdirectory(core)
if(HAS_JZJSEngine)
add_subdirectory(jsengine)
endif()
add_subdirectory(tools)
add_subdirectory(http_server)
add_subdirectory(agent)
add_subdirectory(xml)
add_subdirectory(document)

if(WIN32)
add_subdirectory(widget)
endif()

# JZGame(2D 游戏控件)测试: 暂不进默认构建 (见根 CMakeLists 的 JZGame 注释);
# 启用模块后取消下面注释:
# if(HAS_JZGame)
# add_subdirectory(game)
# endif()

# JZCv(视觉/OCR): 只有模块真建起来(OpenCV + ONNX Runtime 都在)才加这个测试
if(HAS_JZCv)
    add_subdirectory(cv)
endif()

# JZEmulator(GBA 画面控件): 同样只有模块真建起来(找到 mgba_host.dll)才加这个测试
if(HAS_JZEmulator)
    add_subdirectory(emulator)
endif()

add_executable(test_frp test_frp.cpp net/net_test_common.cpp)
target_link_libraries(test_frp JZFramework)

add_executable(test_mcpserver test_mcpserver.cpp)
target_link_libraries(test_mcpserver JZFramework)

# 注: co.test_co 的 ctest 注册在 test/co/CMakeLists.txt 里(那里才是 test_co 的 target 所在)。
# 这里原来又写了一份同名 add_test, 同一个测试被注册两遍 —— ctest 会跑两次(两次都要花时间,
# 失败时也在报告里出现两条)。同名测试不要重复注册。