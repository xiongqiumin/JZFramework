add_executable(test_http_server
    test_static.cpp)

add_executable(test_http
    test_http.cpp)

target_link_libraries(test_http_server JZFramework)
target_link_libraries(test_http JZFramework)
