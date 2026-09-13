add_executable(test_jsengine test_jsengine.cpp)
target_link_libraries(test_jsengine JZFramework)

add_executable(test_jsui test_jsui.cpp)
target_link_libraries(test_jsui JZFramework)
# 把 UI 描述资源拷到构建目录, 供 uiLoader.loadFile 测试
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/timer_ui.xml
               ${CMAKE_CURRENT_BINARY_DIR}/timer_ui.xml COPYONLY)

add_executable(test_jsnet test_jsnet.cpp)
target_link_libraries(test_jsnet JZFramework)
# 让测试能引用 JS 适配层编译产物目录(演示分离式加载 + 固定路径 import)
set(JZ_JS_WRAPPER_DIR "${CMAKE_SOURCE_DIR}/JZJSEngine/js_wrapper")
target_compile_definitions(test_jsnet PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

add_executable(test_jssqlite test_jssqlite.cpp)
target_link_libraries(test_jssqlite JZFramework)
# 同 test_jsnet: 让测试能按固定路径 import 编译产物(sqlite/dist/index.js)
target_compile_definitions(test_jssqlite PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

add_executable(test_jsmodbus test_jsmodbus.cpp)
target_link_libraries(test_jsmodbus JZFramework)
# 同 test_jsnet: 让测试能按固定路径 import 编译产物(modbus/dist/index.js)
target_compile_definitions(test_jsmodbus PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# BTE 上位机(Qt/C++ -> JS 移植)的 协议层 + 传输层 端到端测试。
# 源码目录 = binary/code_agent/runtime/bteserver(纯 JS, 不进编译, 由引擎以 ESM 加载)
add_executable(test_bteserver test_bteserver.cpp)
target_link_libraries(test_bteserver JZFramework)
target_compile_definitions(test_bteserver PRIVATE
    JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}"
    JZ_BTESERVER_DIR="${CMAKE_SOURCE_DIR}/binary/code_agent/runtime/bteserver")

# ---- ctest 注册 ----
add_test(NAME jsengine.test_jsengine COMMAND test_jsengine)
set_tests_properties(jsengine.test_jsengine PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jssqlite COMMAND test_jssqlite)
set_tests_properties(jsengine.test_jssqlite PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsmodbus COMMAND test_jsmodbus)
set_tests_properties(jsengine.test_jsmodbus PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsui COMMAND test_jsui)
set_tests_properties(jsengine.test_jsui PROPERTIES TIMEOUT 60)

add_test(NAME jsengine.test_jsnet COMMAND test_jsnet)
set_tests_properties(jsengine.test_jsnet PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_bteserver COMMAND test_bteserver)
set_tests_properties(jsengine.test_bteserver PROPERTIES TIMEOUT 180)