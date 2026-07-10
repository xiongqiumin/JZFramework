set(JZBLE_SOURCES    
    JZBluetoothDeviceInfo.cpp
    JZBluetoothDeviceInfo.h
    JZBluetoothUuid.cpp
    JZBluetoothUuid.h
    JZLowEnergyCharacteristic.cpp
    JZLowEnergyCharacteristic.h
    JZLowEnergyDescriptor.cpp
    JZLowEnergyDescriptor.h
    JZBleDriver.cpp
    JZBleDriver.h
    JZBLECentral.h
    JZBLECentral.cpp
    JZBLEPeripheral.h
    JZBLEPeripheral.cpp
    JZBLEService.h
    JZBLEService.cpp
    JZBLEDeviceDiscoveryAgent.h
    JZBLEDeviceDiscoveryAgent.cpp
)
convert_to_abs_paths(JZBLE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZBLE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")