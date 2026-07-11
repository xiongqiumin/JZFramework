set(JZGpio_SOURCES
    JZGpio.h
    JZGpio.cpp
    JZGpioDriver.h
    JZGpioDriver.cpp
    JZGpioInfo.h
    JZGpioButton.h
    JZGpioButton.cpp
)

# 平台驱动
if(ESP_PLATFORM)
    list(APPEND JZGpio_SOURCES JZGpioDriverEsp32.h JZGpioDriverEsp32.cpp)
endif()

convert_to_abs_paths(JZGpio_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZGpio_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
