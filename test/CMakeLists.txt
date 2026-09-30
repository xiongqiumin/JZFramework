add_subdirectory(net)
add_subdirectory(co)
add_subdirectory(core)
if(HAS_JZJSEngine)
add_subdirectory(jsengine)
endif()
add_subdirectory(tools)
add_subdirectory(http_server)
add_subdirectory(agent)
add_subdirectory(xml)

if(WIN32)
add_subdirectory(widget)
endif()

# JZCv(视觉/OCR): 只有模块真建起来(OpenCV + ONNX Runtime 都在)才加这个测试
if(HAS_JZCv)
    add_subdirectory(cv)
endif()

add_executable(test_frp test_frp.cpp net/net_test_common.cpp)
target_link_libraries(test_frp JZFramework)

add_executable(test_mcpserver test_mcpserver.cpp)
target_link_libraries(test_mcpserver JZFramework)

# 注: co.test_co 的 ctest 注册在 test/co/CMakeLists.txt 里(那里才是 test_co 的 target 所在)。
# 这里原来又写了一份同名 add_test, 同一个测试被注册两遍 —— ctest 会跑两次(两次都要花时间,
# 失败时也在报告里出现两条)。同名测试不要重复注册。