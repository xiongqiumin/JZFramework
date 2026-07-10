set(JZWIFI_SOURCES
    JZWifiDriver.cpp
    JZWifiDriver.h
    JZWifiStation.cpp
    JZWifiStation.h
    JZWifiAp.cpp
    JZWifiAp.h
    JZWifiScanner.cpp
    JZWifiScanner.h
    JZWifiAutoConnectService.cpp
    JZWifiAutoConnectService.h
    JZWifi.h
)
convert_to_abs_paths(JZWIFI_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZWIFI_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
