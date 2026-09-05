add_executable(test_http_server
    test_static.cpp)

add_executable(test_http_compress
    test_compress.cpp)

add_executable(test_http
    test_http.cpp)

add_executable(test_https
    test_https.cpp)

add_executable(test_route
    test_route.cpp)

add_executable(test_routing
    test_routing.cpp)

target_link_libraries(test_http_server JZFramework)
target_link_libraries(test_http_compress JZFramework)
target_link_libraries(test_http JZFramework)
target_link_libraries(test_https JZFramework)
target_link_libraries(test_route JZFramework)
target_link_libraries(test_routing JZFramework)
