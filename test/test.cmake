#test_lvgl
set(TEST_LVGL_SOURCES test_lvgl.cpp)
convert_to_abs_paths(TEST_LVGL_SOURCES)

#add_executable(test_lvgl ${TEST_LVGL_SOURCES})
#target_link_libraries(test_lvgl JZFramework lvgl)

#test_net (各模块单独测试: TCP / UDP / WebSocket / MQTT, 逐个调用)
set(TEST_NET_SOURCES
    net/test_net.cpp
    net/test_tcp.cpp
    net/test_udp.cpp
    net/test_webSocket.cpp
    net/test_mqtt.cpp
    net/test_http.cpp
    net/test_netServer.cpp)
convert_to_abs_paths(TEST_NET_SOURCES)

add_executable(test_net ${TEST_NET_SOURCES})
target_link_libraries(test_net JZFramework)

#test_co (协程综合测试: 基础库 + 网络 TCP/UDP/WebSocket/HTTP)
# 各子模块独立文件, 由 co/test_co.cpp 统一编排调用 (参考 test/net/test_net.cpp)
set(TEST_CO_SOURCES
    co/test_co.cpp
    co/test_coBasic.cpp
    co/test_coTcp.cpp
    co/test_coUdp.cpp
    co/test_coWebSocket.cpp
    co/test_coHttp.cpp)
convert_to_abs_paths(TEST_CO_SOURCES)

add_executable(test_co ${TEST_CO_SOURCES})
target_link_libraries(test_co JZFramework)