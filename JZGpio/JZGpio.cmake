set(JZGpio_SOURCES
    JZGpio.h
    JZGpio.cpp
    JZGpioDriver.h
    JZGpioDriver.cpp
    JZGpioInfo.h
    JZGpioButton.h
    JZGpioButton.cpp
)

convert_to_abs_paths(JZGpio_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZGpio_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
