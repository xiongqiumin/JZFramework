set(JZSensor_SOURCES
    JZSensorInfo.h
    JZSensor.h
    JZSensor.cpp
    JZSensorDriver.h
    JZSensorDriver.cpp
    JZTemperatureSensor.h
    JZTemperatureSensor.cpp
)

# 平台驱动
if(ESP_PLATFORM)
    list(APPEND JZSensor_SOURCES JZSensorDriverEsp32.h JZSensorDriverEsp32.cpp)
endif()

convert_to_abs_paths(JZSensor_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSensor_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
