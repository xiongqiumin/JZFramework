set(JZSIMULATOR_SOURCES
    JZSimPack.h
    JZSimPack.cpp
    JZSimServer.h
    JZSimServer.cpp
)

# JZSimulator 依赖 JZBle/JZNet/JZGpio，这些模块始终存在
if(HAS_JZBle)
    list(APPEND JZSIMULATOR_SOURCES
        JZBleDriverSimulator.h
        JZBleDriverSimulator.cpp
    )
endif()
if(HAS_JZGpio)
    list(APPEND JZSIMULATOR_SOURCES
        JZGpioDriverSimulator.h
        JZGpioDriverSimulator.cpp
    )
endif()

# 以下模块为可选：仅当对应模块被启用时才编译其模拟驱动
if(HAS_JZWifi)
    list(APPEND JZSIMULATOR_SOURCES
        JZWifiDriverSimulator.h
        JZWifiDriverSimulator.cpp
    )
endif()

if(HAS_JZSensor)
    list(APPEND JZSIMULATOR_SOURCES
        JZSensorDriverSimulator.h
        JZSensorDriverSimulator.cpp
    )
endif()

if(HAS_JZOTA)
    list(APPEND JZSIMULATOR_SOURCES
        JZOTADriverSimulator.h
        JZOTADriverSimulator.cpp
    )
endif()

convert_to_abs_paths(JZSIMULATOR_SOURCES)

target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSIMULATOR_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# 内嵌网页资源: http/simulator.qrc -> 静态库 JZSimulator_resources(暴露 jzres_simulator_init())
jz_add_resources(JZSimulator_resources ${CMAKE_CURRENT_LIST_DIR}/http/simulator.qrc)
target_link_libraries(${JZ_MODULE_TARGET} PRIVATE JZSimulator_resources)

# 去掉外部网络连接(JZNet)，改为内嵌 JZHttpServer 控制。
# 依赖通过 MODULE_DEPENDS_JZSimulator (JZHttpServer / JZJson) 解析。
