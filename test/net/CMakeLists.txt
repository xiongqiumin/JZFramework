

add_executable(test_net test_http.cpp
	test_mqtt.cpp
	test_net.cpp
	test_netServer.cpp
	test_tcp.cpp
	test_udp.cpp
	test_webSocket.cpp)
target_link_libraries(test_net JZFramework)