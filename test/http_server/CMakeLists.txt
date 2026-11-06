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

# 浏览器请求签名流程验证(HMAC-SHA256 + WebCrypto): 需要真实浏览器(Edge/Chrome),
# 没有浏览器的机器上仍会跑完 C++ 自检段。
add_executable(test_http_sign
    test_http_sign.cpp)

# HTTPS 双向认证(mTLS): 服务端用 setSsl(ca, cert, key) 校验客户端证书/私钥.
# 三个用例: 证书有效 -> 200; 不带证书 -> 拒绝; 另一套 CA 签发 -> 拒绝.
add_executable(test_https_mtls
    test_https_mtls.cpp)

target_link_libraries(test_http_server JZFramework)
target_link_libraries(test_http_compress JZFramework)
target_link_libraries(test_http JZFramework)
target_link_libraries(test_https JZFramework)
target_link_libraries(test_route JZFramework)
target_link_libraries(test_routing JZFramework)
target_link_libraries(test_http_sign JZFramework)
target_link_libraries(test_https_mtls JZFramework)

# 自包含(内嵌测试证书, 不依赖外部文件/网络), 可以进 ctest
add_test(NAME http_server.https_mtls COMMAND test_https_mtls)
set_tests_properties(http_server.https_mtls PROPERTIES TIMEOUT 120)
