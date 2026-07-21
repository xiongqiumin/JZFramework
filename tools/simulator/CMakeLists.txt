# JZSimulator 工具: 复用主树 JZFramework 静态库 (不再独立构建)
set(SIM_SOURCES
    JZSimulator.h
    JZSimulator.cpp
    JZSimWebServer.h
    JZSimWebServer.cpp
    main.cpp
)

# simulator 只需与驱动通信的网络包定义, 直接包含 JZSimulator 模块的 JZSimPack (统一数据包)
set(JZSIMULATOR_MODULE_DIR "${JZFRAMEWORK_ROOT_DIR}/JZSimulator")
set(SIM_PACKET_SOURCES
    ${JZSIMULATOR_MODULE_DIR}/JZSimPack.h
    ${JZSIMULATOR_MODULE_DIR}/JZSimPack.cpp
)

add_executable(JZSimulator ${SIM_SOURCES} ${SIM_PACKET_SOURCES})
target_include_directories(JZSimulator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${JZSIMULATOR_MODULE_DIR}
)

# 网页根目录: 由 JZHttpStaticFiles 在运行时静态服务 web/ 下的文件
set(WEB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/web")
file(TO_CMAKE_PATH "${WEB_ROOT}" WEB_ROOT)
target_compile_definitions(JZSimulator PRIVATE JZ_SIM_WEB_ROOT="${WEB_ROOT}")

target_link_libraries(JZSimulator PRIVATE JZFramework)
