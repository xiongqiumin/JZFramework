set(JZSIMULATOR_SOURCES
    JZSimPack.h
    JZSimPack.cpp
    JZSimServer.h
    JZSimServer.cpp
)

# JZSimulator 依赖 JZBle/JZNet/JZGpio，这些模块始终存在
jz_module_exists(JZBle JZSIM_HAS_BLE)
jz_module_exists(JZGpio JZSIM_HAS_GPIO)
if(JZSIM_HAS_BLE)
    list(APPEND JZSIMULATOR_SOURCES
        JZBleDriverSimulator.h
        JZBleDriverSimulator.cpp
    )
endif()
if(JZSIM_HAS_GPIO)
    list(APPEND JZSIMULATOR_SOURCES
        JZGpioDriverSimulator.h
        JZGpioDriverSimulator.cpp
    )
endif()

# 以下模块为可选：仅当对应模块被启用时才编译其模拟驱动
jz_module_exists(JZWifi JZSIM_HAS_WIFI)
if(JZSIM_HAS_WIFI)
    list(APPEND JZSIMULATOR_SOURCES
        JZWifiDriverSimulator.h
        JZWifiDriverSimulator.cpp
    )
endif()

jz_module_exists(JZSensor JZSIM_HAS_SENSOR)
if(JZSIM_HAS_SENSOR)
    list(APPEND JZSIMULATOR_SOURCES
        JZSensorDriverSimulator.h
        JZSensorDriverSimulator.cpp
    )
endif()

jz_module_exists(JZOTA JZSIM_HAS_OTA)
if(JZSIM_HAS_OTA)
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
