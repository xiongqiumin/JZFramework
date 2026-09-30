add_executable(test_jsengine test_jsengine.cpp)
target_link_libraries(test_jsengine JZFramework)

# 以下 UI 相关用例需要自绘 UI(JZWidget)支持, 未启用该模块时整个跳过。
if(HAS_JZWidget)
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
endif()

add_executable(test_jsnet test_jsnet.cpp)
target_link_libraries(test_jsnet JZFramework)
# 让测试能引用 JS 适配层编译产物目录(演示分离式加载 + 固定路径 import)
set(JZ_JS_WRAPPER_DIR "${CMAKE_SOURCE_DIR}/JZJSEngine/js_wrapper")
target_compile_definitions(test_jsnet PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# UI 用例也要它: JZJSWidgetModule 初始化时会加载 widget/dist/index.js(控件族类层: 定义全局控件类),
# 靠 libraryDir 定位 —— 跟前几个包一样按源码树路径直接指过去。
if(TARGET test_jsui)
target_compile_definitions(test_jsui PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")
endif()
if(TARGET test_load_ui)
target_compile_definitions(test_load_ui PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")
endif()

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
# 同 test_jsnet: net 模块注册时要按裸名装载 Web 全局包(events/url/ws/http), 靠 libraryDir 定位;
# worker 引擎继承同一份 libraryDir ⇒ worker 里也有这些全局。
target_compile_definitions(test_jsworker PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# ---- ctest 注册 ----
add_test(NAME jsengine.test_jsengine COMMAND test_jsengine)
set_tests_properties(jsengine.test_jsengine PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jssqlite COMMAND test_jssqlite)
set_tests_properties(jsengine.test_jssqlite PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsmodbus COMMAND test_jsmodbus)
set_tests_properties(jsengine.test_jsmodbus PROPERTIES TIMEOUT 120)

if(HAS_JZWidget)
add_test(NAME jsengine.test_jsui COMMAND test_jsui)
set_tests_properties(jsengine.test_jsui PROPERTIES TIMEOUT 60)
endif()

add_test(NAME jsengine.test_jsnet COMMAND test_jsnet)
set_tests_properties(jsengine.test_jsnet PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jsworker COMMAND test_jsworker)
set_tests_properties(jsengine.test_jsworker PROPERTIES TIMEOUT 180)

# load_ui: 一份数据一份测试(运行时各跑一份, 互不影响)。新增数据文件时在此加一行即可。
if(HAS_JZWidget)
add_test(NAME jsengine.test_load_ui.basic COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/basic_window.xml)
set_tests_properties(jsengine.test_load_ui.basic PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.menubar COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/main_menubar.xml)
set_tests_properties(jsengine.test_load_ui.menubar PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.containers COMMAND test_load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/containers.xml)
set_tests_properties(jsengine.test_load_ui.containers PROPERTIES TIMEOUT 60)
endif()

# computer-use: JS 侧端到端(**虚拟桌面**, 无真实窗口 ⇒ 可无头跑)。
#   TS 包(computeruse/dist) → __jz.computeruse.* → JZComputerUse → JZVirtualDevice → 控件。
# 依赖: JZWidget(虚拟桌面/控件) + CU 那两个源文件(与 binary/code_agent/runtime 同一套依赖)。
if(HAS_JZWidget)
# (JZJSComputerUseModule 与 JZComputerUse 都不在 JZFramework.lib 里 —— 只有 code_runtime
#  和本测试显式编它们, 与 binary/code_agent/runtime/CMakeLists.txt 同一套依赖。)
add_executable(test_jscu
    test_jscu.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZScreenShot.cpp)
target_include_directories(test_jscu PRIVATE ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent)
target_link_libraries(test_jscu JZFramework)
# 虚拟桌面后端 (JZComputerUse 的 setVirtualDesktop 用它)
target_link_libraries(test_jscu jz_virtual_device)
target_compile_definitions(test_jscu PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# 像素族(颜色匹配)要走 OpenCV: 装上就开, 没装自动跳过(JS 侧那条断言会打印 [skip])
jz_opencv_enable(test_jscu)

add_test(NAME jsengine.test_jscu COMMAND test_jscu)
set_tests_properties(jsengine.test_jscu PROPERTIES TIMEOUT 120)
endif()