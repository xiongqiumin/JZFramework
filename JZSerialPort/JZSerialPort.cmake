set(JZSerialPort_SOURCES    
    JZSerialPort.h
    JZSerialPort.cpp
	JZSerialPortDriver.h
	JZSerialPortDriver.cpp
)

convert_to_abs_paths(JZSerialPort_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSerialPort_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")