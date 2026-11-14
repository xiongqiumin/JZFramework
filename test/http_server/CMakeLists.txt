# ---------------------------------------------------------------------------
# HTTP 服务端测试: 合成一个 test_http
#
#   每个用例仍然是**一个进程**(ctest 粒度不变, 崩一个不影响别的), 但只链接一次
#   JZFramework、解决方案里只多一个目标。用法: test_http <case>, 见 main.cpp。
#
#   自包含(本机回环, 不依赖外部服务/浏览器)的用例都进 ctest。
#   test_http_sign 需要真实浏览器跑 WebCrypto 签名, 保持独立、不进 ctest。
# ---------------------------------------------------------------------------
add_executable(test_http
    main.cpp
    test_static.cpp
    test_http.cpp
    test_compress.cpp
    test_cookie.cpp
    test_peer.cpp
    test_route.cpp
    test_routing.cpp
    test_https.cpp
    test_https_mtls.cpp)
target_link_libraries(test_http JZFramework)

# 浏览器请求签名流程验证(HMAC-SHA256 + WebCrypto): 需要真实浏览器(Edge/Chrome),
# 没有浏览器的机器上仍会跑完 C++ 自检段。运行参数见该文件头注释。
add_executable(test_http_sign
    test_http_sign.cpp)
target_link_libraries(test_http_sign JZFramework)

# ---- ctest: 一条用例一条, 名字沿用原来的 http_server.* ----
# (route / routing / compress 是合并后第一次跑起来的既有用例, 暴露的三个 bug 已修:
#  路由 <id> 贪婪跨段、catch-all 参数名多带冒号、响应头含二进制 body 时 keep-alive 误判)
foreach(_case static sse_echo compress cookie peer route routing https https_mtls)
    add_test(NAME http_server.${_case} COMMAND test_http ${_case})
    set_tests_properties(http_server.${_case} PROPERTIES TIMEOUT 120)
endforeach()
