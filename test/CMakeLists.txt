add_subdirectory(net)
add_subdirectory(co)
add_subdirectory(core)
add_subdirectory(jsengine)
add_subdirectory(widget)

add_executable(test_frp test_frp.cpp)
target_link_libraries(test_frp JZFramework)