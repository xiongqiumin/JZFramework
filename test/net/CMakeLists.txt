

add_executable(test_net net_test_common.cpp
	test_http.cpp
	test_mqtt.cpp
	test_net.cpp
	test_netServer.cpp
	test_tcp.cpp
	test_udp.cpp
	test_webSocket.cpp)
target_link_libraries(test_net JZFramework)

# ---- ctest 注册: 仅自包含的 test_net(local 模式, 单/多线程各15s) ----
# test_tcp_bench/test_mg* 为基准程序、test_proxy/test_net_compress 依赖本地 python 服务, 不注册
add_test(NAME net.test_net COMMAND test_net)
set_tests_properties(net.test_net PROPERTIES TIMEOUT 600 LABELS "slow")

# TCP client 上下行压测 (独立程序, 配合 iperf3/nc 等 Linux 压测工具)
add_executable(test_tcp_bench test_tcp_bench.cpp)
target_link_libraries(test_tcp_bench JZFramework)


# 纯 mongoose.c TCP 基准测试 (独立程序, 自带一份 mongoose.c 实现, 不链接 JZFramework,
# 与 JZNet 库内部的 mongoose 全局状态完全隔离)
# 用法: test_mg --mode <server|client|local> [--server <ip>] [--port <port>] [--time <ms>]
add_executable(test_mg test_mg.cpp ${CMAKE_SOURCE_DIR}/JZNet/mongoose.c)
target_include_directories(test_mg PRIVATE ${CMAKE_SOURCE_DIR}/JZNet)

add_executable(test_mg_event test_mg_event.cpp ${CMAKE_SOURCE_DIR}/JZNet/mongoose.c)
target_include_directories(test_mg_event PRIVATE ${CMAKE_SOURCE_DIR}/JZNet)

add_executable(test_mg_postevent test_mg_postevent.cpp)
target_link_libraries(test_mg_postevent JZFramework)

# HTTP/HTTPS 代理测试: 验证 http 目标经代理转发、https 目标经代理 CONNECT 隧道
# 运行前通过环境变量注入代理: http_proxy / https_proxy (同 libcurl)
add_executable(test_proxy test_proxy.cpp)
target_link_libraries(test_proxy JZFramework)

# 压缩响应测试: 本地 server 返回 gzip / deflate / 明文 响应，client 校验解压结果（端到端验证 HTTP Content-Encoding 支持）
add_executable(test_net_compress test_net_compress.cpp)
target_link_libraries(test_net_compress JZFramework)