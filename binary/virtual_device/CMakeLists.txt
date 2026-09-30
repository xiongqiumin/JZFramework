# ---------------------------------------------------------------------------
# virtual_device —— 虚拟桌面 (虚拟显示器 + 窗口管理器) 与它的宿主程序
#
# 它是**应用侧**的东西 (不在框架核心 JZWidget 里): 虚拟窗口的后端 / 窗口对象 /
# 桌面管理器 / 显示控件 / 虚拟进程都在这里。JZWindow 只保留"平台后端"这一个抽象 ⇒
# "多一个平台 = 多一个后端实现", 核心库一行都不用改。
#
# 需要虚拟桌面的目标 (computer-use 的虚拟后端 / 测试) 直接:
#   target_link_libraries(<target> PRIVATE jz_virtual_device)
# ---------------------------------------------------------------------------
add_library(jz_virtual_device STATIC
    JZVirtualWindow.h
    JZVirtualWindow.cpp
    JZVirtualWindowBackend.h
    JZVirtualWindowBackend.cpp
    JZVirtualDevice.h
    JZVirtualDevice.cpp
    JZVirtualThread.h      # 跨线程同步调用 (jzRunOnThreadSync) 的小工具头
    JZVirtualScreenView.h
    JZVirtualScreenView.cpp
)
target_link_libraries(jz_virtual_device PUBLIC JZFramework)
# 两种引用方式都支持:
#   #include "JZVirtualDevice.h"                      (本目录, 与 JZWidget 的头一样)
#   #include "binary/virtual_device/JZVirtualDevice.h" (仓库根相对, 与 JZComputerUse 一致)
target_include_directories(jz_virtual_device PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${JZFRAMEWORK_ROOT_DIR}")

# ---------------------------------------------------------------------------
# jz_virtual_process —— 虚拟进程 (一个窗口一个线程 + 一份 JS 运行时)
#
# 它要 JZEngineRuntime (内含 JZJSEngine), 而那个运行时要拉一整套"宿主能力"源文件
# (与 binary/code_agent/runtime 同一套依赖): 抓屏/OCR 桥/computer-use 模块。
# 那几个源文件不在 JZFramework.lib 里 ⇒ 在这里编进本库 (谁需要谁编, 见根 README 的约定)。
#   方向: jz_virtual_process -> jz_virtual_device (单向; 桌面库不认识引擎)
# ---------------------------------------------------------------------------
set(JZVirtualProcess_SOURCES
    JZVirtualProcess.h
    JZVirtualProcess.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZEngineRuntime.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZSourceMap.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZExeResource.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/code_agent/JZScreenShot.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
)
add_library(jz_virtual_process STATIC ${JZVirtualProcess_SOURCES})
target_link_libraries(jz_virtual_process PUBLIC jz_virtual_device)
target_include_directories(jz_virtual_process PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${JZFRAMEWORK_ROOT_DIR}"
    "${JZFRAMEWORK_ROOT_DIR}/binary/code_agent")   # JZScreenShot.h 等按裸名被引用

# 宿主程序: VirtualVm 风格 —— 菜单栏 + 中央滚动区, 滚动区里是 800x600 的虚拟屏;
# 菜单里可以"新建进程窗口"(起一个线程 + 一份 JS 运行时, 里面建窗口)
add_executable(virtual_device main.cpp)
target_link_libraries(virtual_device PRIVATE jz_virtual_process)
set_target_properties(virtual_device PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")

# 进程里的引擎按 **exe 目录** 找 JS 包 (JZEngineRuntime: <appDir>/jz_js/<pkg>/dist/...) ——
# 把控件类层部署到 exe 边上, 于是 JS 里 new JZWindow/JZPushButton 才有类可用
# (与 code_agent 的 jz_engine/jz_js 同一套约定, 这里只拷需要的那个包)。
add_custom_command(TARGET virtual_device POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/js_wrapper/widget/dist"
            "$<TARGET_FILE_DIR:virtual_device>/jz_js/widget/dist"
    COMMENT "Deploy widget JS class layer next to virtual_device.exe")
