# 资源: res.qrc (prefix=/data) -> 静态库 test_tools_resources
jz_add_resources(test_tools_resources res.qrc)

# Proto: sample.proto -> 静态库 test_tools_proto (生成 jz_test_tools_proto.h)
jz_add_proto_json(test_tools_proto sample.proto)

add_executable(test_tools test_tools.cpp)
target_link_libraries(test_tools JZFramework test_tools_resources test_tools_proto)

# ---- ctest 注册 ----
add_test(NAME tools.test_tools COMMAND test_tools)
set_tests_properties(tools.test_tools PROPERTIES TIMEOUT 60)
