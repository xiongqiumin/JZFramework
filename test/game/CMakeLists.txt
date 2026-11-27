# JZGame 测试目录 (test/CMakeLists.txt 里 HAS_JZGame 才进来)
#
# 注: JZGame 是**单独 DLL**, 所以这里每个 exe 都链 JZGame (它 PUBLIC 链 JZFramework,
# 框架的头/宏会一起带出); 运行期 JZGame.cmake 已把 JZGame[d].dll 拷到 test/bin/<配置>。
add_subdirectory(shooter)
add_subdirectory(danmaku)
add_subdirectory(scroll)
add_subdirectory(bench)
