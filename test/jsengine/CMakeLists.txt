# ---------------------------------------------------------------------------
# JS 引擎测试: 合成一个 test_jsengine
#
#   每个用例仍然是一个进程(test_jsengine <case>), ctest 粒度不变;
#   只链接一次 JZFramework、解决方案里只多一个目标。
#
#   test_jscu 仍独立: 它要多编 JZJSComputerUseModule / JZComputerUse / JZScreenShot
#   并链接 jz_virtual_device + OpenCV, 不该把重依赖加给其它用例。
# ---------------------------------------------------------------------------
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/JZJSEngine/js_wrapper" JZ_JS_WRAPPER_DIR)

add_executable(test_jsengine
    main.cpp
    test_jsengine.cpp
    test_jsnet.cpp
    test_jssqlite.cpp
    test_jsmodbus.cpp
    test_jsworker.cpp)
target_link_libraries(test_jsengine JZFramework)
# 让用例能按固定路径 import js_wrapper 的编译产物(net/sqlite/modbus/worker 都要)
target_compile_definitions(test_jsengine PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# 以下 UI 相关用例需要自绘 UI(JZWidget) 支持, 未启用该模块时整个跳过。
if(HAS_JZWidget)
    target_sources(test_jsengine PRIVATE test_jsui.cpp test_load_ui.cpp)
    target_compile_definitions(test_jsengine PRIVATE JZ_TEST_HAS_WIDGET=1)
    # 把 UI 描述资源拷到构建目录, 供 uiLoader.loadFile 测试
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/timer_ui.xml
                   ${CMAKE_CURRENT_BINARY_DIR}/timer_ui.xml COPYONLY)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ---- ctest 注册(名字沿用原来的 jsengine.*) ----
add_test(NAME jsengine.test_jsengine COMMAND test_jsengine engine)
set_tests_properties(jsengine.test_jsengine PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jssqlite COMMAND test_jsengine sqlite)
set_tests_properties(jsengine.test_jssqlite PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsmodbus COMMAND test_jsengine modbus)
set_tests_properties(jsengine.test_jsmodbus PROPERTIES TIMEOUT 120)

add_test(NAME jsengine.test_jsnet COMMAND test_jsengine net)
set_tests_properties(jsengine.test_jsnet PROPERTIES TIMEOUT 300)

add_test(NAME jsengine.test_jsworker COMMAND test_jsengine worker)
set_tests_properties(jsengine.test_jsworker PROPERTIES TIMEOUT 180)

if(HAS_JZWidget)
add_test(NAME jsengine.test_jsui COMMAND test_jsengine ui)
set_tests_properties(jsengine.test_jsui PROPERTIES TIMEOUT 60)

# load_ui: 一份数据一份测试(运行时各跑一份, 互不影响)。新增数据文件时在此加一行即可。
add_test(NAME jsengine.test_load_ui.basic
         COMMAND test_jsengine load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/basic_window.xml)
set_tests_properties(jsengine.test_load_ui.basic PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.menubar
         COMMAND test_jsengine load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/main_menubar.xml)
set_tests_properties(jsengine.test_load_ui.menubar PROPERTIES TIMEOUT 60)
add_test(NAME jsengine.test_load_ui.containers
         COMMAND test_jsengine load_ui ${CMAKE_CURRENT_BINARY_DIR}/data/containers.xml)
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
    ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/JZScreenShot.cpp)
target_include_directories(test_jscu PRIVATE ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop)
target_link_libraries(test_jscu JZFramework)
# 虚拟桌面后端 (JZComputerUse 的 setVirtualDesktop 用它)
target_link_libraries(test_jscu jz_virtual_device)
target_compile_definitions(test_jscu PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# 像素族(颜色匹配)要走 OpenCV: 装上就开, 没装自动跳过(JS 侧那条断言会打印 [skip])
jz_opencv_enable(test_jscu)

add_test(NAME jsengine.test_jscu COMMAND test_jscu)
set_tests_properties(jsengine.test_jscu PROPERTIES TIMEOUT 120)
endif()
