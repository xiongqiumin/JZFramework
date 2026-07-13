#test_lvgl
set(TEST_LVGL_SOURCES test_lvgl.cpp)
convert_to_abs_paths(TEST_LVGL_SOURCES)

add_executable(test_lvgl ${TEST_LVGL_SOURCES})
target_link_libraries(test_lvgl JZFramework lvgl)

#test_mainwindow
set(TEST_ESP_SOURCES test_esp.cpp main_window.cpp main_window.h)
convert_to_abs_paths(TEST_ESP_SOURCES)

add_executable(test_esp ${TEST_ESP_SOURCES})
target_link_libraries(test_esp JZFramework lvgl)

#test_net (各模块单独测试: TCP / UDP / WebSocket / MQTT, 逐个调用)
set(TEST_NET_SOURCES
    test/net/test_net.cpp
    test/net/test_tcp.cpp
    test/net/test_udp.cpp
    test/net/test_webSocket.cpp
    test/net/test_mqtt.cpp
    test/net/test_http.cpp
    test/net/test_netServer.cpp)
convert_to_abs_paths(TEST_NET_SOURCES)

add_executable(test_net ${TEST_NET_SOURCES})
target_link_libraries(test_net JZFramework)