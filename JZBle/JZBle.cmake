set(JZBLE_SOURCES    
    JZBLEDeviceInfo.cpp
    JZBLEDeviceInfo.h
    JZBluetoothUuid.cpp
    JZBluetoothUuid.h
    JZLowEnergyCharacteristic.cpp
    JZLowEnergyCharacteristic.h
    JZLowEnergyDescriptor.cpp
    JZLowEnergyDescriptor.h
    JZBleDriver.cpp
    JZBleDriver.h
    JZBLEController.h
    JZBLEController.cpp
    JZBLEPeripheral.h
    JZBLEPeripheral.cpp
    JZBLEService.h
    JZBLEService.cpp
    JZBLEDeviceDiscoveryAgent.h
    JZBLEDeviceDiscoveryAgent.cpp
)

# 平台驱动
if(ESP_PLATFORM)
    list(APPEND JZBLE_SOURCES JZBleDriverEsp32.cpp)
endif()

convert_to_abs_paths(JZBLE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZBLE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")