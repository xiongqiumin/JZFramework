

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