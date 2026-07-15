cmake_minimum_required(VERSION 3.10)
project(JZSimulator)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

# ======================== Qt ========================
# Qt5.15 安装路径 (msvc2015_64), 可用 -DQt5_DIR 覆盖
set(Qt5_DIR "C:/Qt5.15/5.15.2/msvc2015_64/lib/cmake/Qt5" CACHE PATH "Qt5 cmake 目录")
find_package(Qt5 REQUIRED COMPONENTS Widgets)

# ======================== JZFramework 库 ========================
# 独立引用外部 JZFramework.cmake, 编译 JZFramework 静态库 (不与主 CMakeLists 关联)
# 仅启用 simulator 所需的底层模块, 不引入 JZSimulator 模块 (避免拉入驱动侧代码)
set(JZFRAMEWORK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${JZFRAMEWORK_ROOT}/JZFramework.cmake")
jzframework_use_modules(JZCore JZNet)

# ======================== 直接引用 JZSimulator 的网络包源码 ========================
# simulator 界面只需与驱动通信的网络包定义, 直接包含源文件, 不编译整个 JZSimulator 模块
set(JZSIMULATOR_MODULE_DIR "${JZFRAMEWORK_ROOT}/JZSimulator")
set(SIM_PACKET_SOURCES
    ${JZSIMULATOR_MODULE_DIR}/JZBleSimPack.h
    ${JZSIMULATOR_MODULE_DIR}/JZBleSimPack.cpp
)

# ======================== 构建 simulator 可执行程序 ========================
set(SIM_SOURCES
    JZSimulator.h
    JZSimulator.cpp
    main.cpp
)
convert_to_abs_paths(SIM_SOURCES)

add_executable(JZSimulator ${SIM_SOURCES} ${SIM_PACKET_SOURCES})
target_include_directories(JZSimulator PRIVATE ${JZSIMULATOR_MODULE_DIR})
target_link_libraries(JZSimulator PRIVATE JZFramework Qt5::Widgets)
