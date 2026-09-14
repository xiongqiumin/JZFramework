add_executable(test_jsengine test_jsengine.cpp)
target_link_libraries(test_jsengine JZFramework)

add_executable(test_jsui test_jsui.cpp)
target_link_libraries(test_jsui JZFramework)
# 把 UI 描述资源拷到构建目录, 供 uiLoader.loadFile 测试
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/timer_ui.xml
               ${CMAKE_CURRENT_BINARY_DIR}/timer_ui.xml COPYONLY)

# load_ui: 加载一份 UI 描述 -> 解析 + createUi 运行时构造 + 校验控件树(菜单栏/菜单/动作)。
# 每次运行只吃一份数据文件(由 add_test 通过参数指定), 一份失败不影响其它数据。
add_executable(test_load_ui test_load_ui.cpp)
target_link_libraries(test_load_ui JZFramework)
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

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

add_executable(test_jsworker test_jsworker.cpp)
target_link_libraries(test_jsworker JZFramework)

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

add_test(NAME jsengine.test_jsworker COMMAND test_jsworker)
set_tests_properties(jsengine.test_jsworker PROPERTIES TIMEOUT 180)

# load_ui: 一份数据一份测试(运行时各跑一份, 互不影响)。新增数据文件时在此加一行即可。
add_test(NAME jsengine.test_load_ui.basic COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/basic_window.xml)
set_tests_properties(jsengine.test_load_ui.basic PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.menubar COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/main_menubar.xml)
set_tests_properties(jsengine.test_load_ui.menubar PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.containers COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/containers.xml)
set_tests_properties(jsengine.test_load_ui.containers PROPERTIES TIMEOUT 60)