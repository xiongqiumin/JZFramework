set(JZSerialPort_SOURCES    
    JZSerialPort.h
    JZSerialPort.cpp
	JZSerialPortInfo.h
	JZSerialPortDriver.h
	JZSerialPortDriver.cpp
)

# 平台相关驱动，仅编译对应平台的实现
if(ESP_PLATFORM)
    list(APPEND JZSerialPort_SOURCES
        JZSerialDriverEsp32.h
        JZSerialDriverEsp32.cpp
    )
elseif(WIN32)
    list(APPEND JZSerialPort_SOURCES
        JZSerialDriverWindows.h
        JZSerialDriverWindows.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND JZSerialPort_SOURCES
        JZSerialDriverLinux.h
        JZSerialDriverLinux.cpp
    )
endif()

convert_to_abs_paths(JZSerialPort_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSerialPort_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
