cmake_minimum_required(VERSION 3.10)
project(JZSimulator)

set(CMAKE_CXX_STANDARD 17)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

# ======================== JZFramework 库 ========================
# 独立引用外部 JZFramework.cmake, 编译 JZFramework 静态库 (不与主 CMakeLists 关联)
# 界面改由 JZHttpServer + 网页提供, 仅需网络与 HTTP 服务相关模块
set(JZFRAMEWORK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
include("${JZFRAMEWORK_ROOT}/JZFramework.cmake")
jzframework_use_modules(JZCore JZNet JZHttpServer JZJson)

# ======================== 直接引用 JZSimulator 的网络包源码 ========================
# simulator 只需与驱动通信的网络包定义, 直接包含源文件, 不编译整个 JZSimulator 模块
set(JZSIMULATOR_MODULE_DIR "${JZFRAMEWORK_ROOT}/JZSimulator")
set(SIM_PACKET_SOURCES
    ${JZSIMULATOR_MODULE_DIR}/JZBleSimPack.h
    ${JZSIMULATOR_MODULE_DIR}/JZBleSimPack.cpp
)

# ======================== 构建 simulator 可执行程序 ========================
set(SIM_SOURCES
    JZSimulator.h
    JZSimulator.cpp
    JZSimWebServer.h
    JZSimWebServer.cpp
    main.cpp
)
convert_to_abs_paths(SIM_SOURCES)

add_executable(JZSimulator ${SIM_SOURCES} ${SIM_PACKET_SOURCES})
target_include_directories(JZSimulator PRIVATE ${JZSIMULATOR_MODULE_DIR})

# 网页根目录: 由 JZHttpStaticFiles 在运行时静态服务 web/ 下的文件
set(WEB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/web")
file(TO_CMAKE_PATH "${WEB_ROOT}" WEB_ROOT)
target_compile_definitions(JZSimulator PRIVATE JZ_SIM_WEB_ROOT="${WEB_ROOT}")

target_link_libraries(JZSimulator PRIVATE JZFramework)
