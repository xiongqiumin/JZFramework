

add_executable(test_net net_test_common.cpp
	test_http.cpp
	test_mqtt.cpp
	test_net.cpp
	test_netServer.cpp
	test_tcp.cpp
	test_udp.cpp
	test_webSocket.cpp)
target_link_libraries(test_net JZFramework)

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