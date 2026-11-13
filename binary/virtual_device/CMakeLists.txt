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
    JZVirtualDeviceHttp.h    # HTTP 接口: 加载程序/截图/鼠标/键盘 (把桌面当容器)
    JZVirtualDeviceHttp.cpp
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
    ${JZFRAMEWORK_ROOT_DIR}/binary/js_runtime/JZEngineRuntime.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/JZSourceMap.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/JZExeResource.cpp
    ${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop/JZScreenShot.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZComputerUse/JZComputerUse.cpp
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSComputerUseModule.cpp
    # OpenCV 图像处理桥（__jz.cv）：和 JZComputerUse 一样"谁需要谁编"。编入 OpenCV 与否由
    # 上面 jz_opencv_enable(jz_virtual_process) 决定 —— 容器里已经有 OpenCV(DLL 在 exe 边),
    # 所以容器里的脚本也能直接做滤波/形态学这类像素处理。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSCvModule.cpp
    # ONNX Runtime 推理桥（__jz.onnx）：让容器里的脚本也能跑 .onnx 模型。
    # 与 cv 一样"谁需要谁编"; 编入 ORT 与否由下面的 jz_onnx_enable 决定。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSOnnxModule.cpp
    # 音频录放桥（__jz.audio）：让容器里的脚本也能录音/放音。
    ${JZFRAMEWORK_ROOT_DIR}/JZJSEngine/JZJSAudioModule.cpp
    )
add_library(jz_virtual_process STATIC ${JZVirtualProcess_SOURCES})
target_link_libraries(jz_virtual_process PUBLIC jz_virtual_device)
target_include_directories(jz_virtual_process PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${JZFRAMEWORK_ROOT_DIR}"
    "${JZFRAMEWORK_ROOT_DIR}/binary/js_runtime"                 # JZEngineRuntime.h
    "${JZFRAMEWORK_ROOT_DIR}/binary/agent_cli/desktop")        # JZScreenShot.h 等按裸名被引用

# 容器里跑的 JS 程序也可能 new JZEmulatorView（JZEngineRuntime 就在本库里），所以这里接线。
# 静态库只接头/宏/链接，DLL 由最终 exe（virtual_device / code_runtime）去拷。
if(HAS_JZEmulator)
    jz_emulator_enable(jz_virtual_process)
endif()

# computer use 的**像素族模板匹配**（matchImage）要 OpenCV：找到就接, 没装自动跳过。
# ⚠ 这一份 JZComputerUse.cpp 编在**本库**里（见上面的 SOURCES），所以宏/头要加在本库上;
#   运行库 DLL 由 exe 那侧拷（见下面 virtual_device 的 jz_opencv_enable）。
#   颜色查找 matchColor **不依赖 OpenCV**（纯 C++ 实现, 快且能取到"最大的那块"）。
jz_opencv_enable(jz_virtual_process)

# ONNX Runtime（__jz.onnx）：容器里的脚本跑 .onnx 模型要用。宏/头加在本库上（那份
# JZJSOnnxModule.cpp 编在这里），运行库 DLL 由下面的 exe 那侧拷（与 OpenCV 同款分工）。
jz_onnx_enable(jz_virtual_process)

# OCR（__jz.computeruse 的 initOcr/ocrReady/ocrTexts/match{kind:'ocr'}）：让容器里的脚本
# 也能"读屏幕上的字"。要的是 **JZCv 静态库 + JZ_CU_OCR=1**（那份 JZComputerUse.cpp 编在本库里）。
# ⚠ 只有 OpenCV + ONNX Runtime 都在时 JZCv 才存在 ⇒ 整块挂在 HAS_JZCv 里。
#   模型目录不在 exe 边: 用 JZ_CU_OCR_MODEL 环境变量指(或 initOcr({modelRoot}))。
if(HAS_JZCv)
    target_link_libraries(jz_virtual_process PRIVATE JZCv)
    target_compile_definitions(jz_virtual_process PRIVATE JZ_CU_OCR=1)
endif()

# 宿主程序: VirtualVm 风格 —— 菜单栏 + 中央滚动区, 滚动区里是 800x600 的虚拟屏;
# 菜单里可以"新建进程窗口"(起一个线程 + 一份 JS 运行时, 里面建窗口)
add_executable(virtual_device main.cpp)
target_link_libraries(virtual_device PRIVATE jz_virtual_process)
# 输出目录不在这里定: virtual_device 是**发布** exe, 名字在 binary/CMakeLists.txt 末尾那张
# 发布清单里 —— 统一输出到 <build>/binary/bin/<CONFIG>, 与 code_agent 那几个 exe 同目录。

# 游戏模拟器控件：exe 这边负责把 mgba_host.dll 与它的 MinGW 运行库拷到边上
# （静态库那边只接线，见上面 jz_virtual_process 的说明）
if(HAS_JZEmulator)
    jz_emulator_enable(virtual_device)
endif()

# computer use 的像素族（matchImage 模板匹配）也要 OpenCV ⇒ exe 这边把 opencv_world DLL
# 按配置拷到边上（宏/头加在 jz_virtual_process 上, 因为那份 JZComputerUse.cpp 编在那里）
jz_opencv_enable(virtual_device)
# ONNX Runtime 的运行库同理（宏/头在上面 jz_virtual_process 那侧）
jz_onnx_enable(virtual_device)

# 控件类层(widget/dist/index.js)由 **copy_jz_js** 一起部署: 进程里的引擎按
# JZEngineRuntime 的 libraryDir 找包, 其中 <appDir>/js_wrapper 就是那棵部署树
# (见 binary/CMakeLists.txt), 里面每个包一份 —— 所以这里**不再**单独往 exe 边平铺一份
# widget/dist(那会和 js_wrapper/widget/dist 逐字节重复; 2026-10-04 去掉)。
# ⚠ 只有"旁边没有部署树的测试 exe"才需要自带一份, 见 test/widget/virtual/CMakeLists.txt。

# 但它**能加载真程序**: `--program app.js` / `POST /api/programs {file}` 是按 ESM 模块加载的,
# 程序里会 import 运行期包 (fs/path/http...)。所以这里跟 code_agent 一样, 要 exe 边有整套
# js_wrapper 树 —— 少了它, 程序只能 import 到 widget, 一 import fs 就静默失败
# (报 "unknown error", 很难查)。
# ⚠ 部署命令不在这里: 本 exe 与 code_agent 那几个现在输出到**同一个**目录, 两处各写一份
#   js_wrapper 会互相拆台(package.py 每次整棵删掉再重铺)。整个 binary/ 只留一处 ——
#   见 binary/CMakeLists.txt 的 copy_jz_js(含 "dist 要先 tsc -b js_wrapper" 等注意事项)。
add_dependencies(virtual_device copy_jz_js)
