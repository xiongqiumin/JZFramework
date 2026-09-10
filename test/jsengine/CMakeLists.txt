add_executable(test_jsengine test_jsengine.cpp)
target_link_libraries(test_jsengine JZFramework)

add_executable(test_jsui test_jsui.cpp)
target_link_libraries(test_jsui JZFramework)

add_executable(test_jsnet test_jsnet.cpp)
target_link_libraries(test_jsnet JZFramework)
# 让测试能引用 JS 适配层编译产物目录(演示分离式加载 + 固定路径 import)
set(JZ_JS_WRAPPER_DIR "${CMAKE_SOURCE_DIR}/JZJSEngine/js_wrapper")
target_compile_definitions(test_jsnet PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

add_executable(test_jssqlite test_jssqlite.cpp)
target_link_libraries(test_jssqlite JZFramework)
# 同 test_jsnet: 让测试能按固定路径 import 编译产物(sqlite/dist/index.js)
target_compile_definitions(test_jssqlite PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# ---- ctest 注册 ----
add_test(NAME jsengine.test_jsengine COMMAND test_jsengine)
set_tests_properties(jsengine.test_jsengine PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jssqlite COMMAND test_jssqlite)
set_tests_properties(jsengine.test_jssqlite PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsui COMMAND test_jsui)
set_tests_properties(jsengine.test_jsui PROPERTIES TIMEOUT 60)

add_test(NAME jsengine.test_jsnet COMMAND test_jsnet)
set_tests_properties(jsengine.test_jsnet PROPERTIES TIMEOUT 300)
