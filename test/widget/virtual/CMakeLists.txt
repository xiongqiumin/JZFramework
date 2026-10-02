add_executable(test_virtual_device main.cpp)
target_link_libraries(test_virtual_device JZFramework)
# 虚拟桌面 (虚拟显示器 + 窗口管理器) 在 binary/virtual_device, 是应用侧的库;
# 虚拟进程 (一个窗口一个线程 + 一份 JS 运行时) 在 jz_virtual_process (它拉一整套运行时源文件)
target_link_libraries(test_virtual_device jz_virtual_process)

# ⚠ jz_virtual_process 里现在编着"**编入 OpenCV / ONNX Runtime**"版的模块（__jz.cv、__jz.onnx，
#   以及 JZCv 的 OCR 内核），而 jz_opencv_enable / jz_onnx_enable 是**属性式**接线、不随库传播
#   ⇒ 最终 exe 必须自己再接一次，否则链接期一堆 cv::* / OrtGetApiBase 未解析（实测 106 个）。
#   与 binary/virtual_device 的 virtual_device exe、code_agent/runtime 的 code_runtime 同款。
jz_opencv_enable(test_virtual_device)
jz_onnx_enable(test_virtual_device)
# 同理: jz_virtual_process 里还接了**游戏模拟器控件**（见它的 CMakeLists），jzRegisterEmulatorView
# 的符号也得由最终 exe 带上 —— 这处是既有遗漏（漏了它这个 target 一直链接失败）。
if(HAS_JZEmulator)
    jz_emulator_enable(test_virtual_device)
endif()

# 末段用例在引擎里 new 控件/窗口(验证"JS 建的窗口也是虚拟的"), 控件类层靠它定位:
# widget/dist/index.js 是引擎按 libraryDir 加载的包(见 JZJSEngine 的 builtinModules)。
set(JZ_JS_WRAPPER_DIR "${CMAKE_SOURCE_DIR}/JZJSEngine/js_wrapper")
target_compile_definitions(test_virtual_device PRIVATE JZ_JS_WRAPPER_DIR="${JZ_JS_WRAPPER_DIR}")

# 虚拟进程里的引擎是**按 exe 目录**找 JS 包 (JZEngineRuntime: <appDir>/jz_js/...),
# 所以把控件类层部署到测试 exe 边上 —— 与 code_agent 的 jz_engine/jz_js 同一套约定。
add_custom_command(TARGET test_virtual_device POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZ_JS_WRAPPER_DIR}/widget/dist"
            "$<TARGET_FILE_DIR:test_virtual_device>/jz_js/widget/dist"
    COMMENT "Deploy widget JS class layer next to test_virtual_device.exe")

# 不创建真实窗口 => 可以进 ctest
add_test(NAME virtual_device COMMAND test_virtual_device)
