add_subdirectory(net)
add_subdirectory(co)
add_subdirectory(core)
add_subdirectory(jsengine)
add_subdirectory(tools)
add_subdirectory(http_server)
add_subdirectory(agent)
add_subdirectory(widget)


add_executable(test_frp test_frp.cpp net/net_test_common.cpp)
target_link_libraries(test_frp JZFramework)