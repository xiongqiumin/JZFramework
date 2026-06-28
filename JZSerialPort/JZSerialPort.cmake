set(JZSerialPort_SOURCES    
    JZSerialPort.h
    JZSerialPort.cpp
)

if(WIN32)
    # Windows平台：添加Windows特有的音频实现文件
    list(APPEND JZSerialPort_SOURCES
        JZSerialPort_win.cpp
		JZSerialPortInfo_win.cpp
    )

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux平台：添加Linux特有的音频实现文件
    list(APPEND JZSerialPort_SOURCES
        JZSerialPort_linux.cpp
		JZSerialPortInfo_linux.cpp
    )
endif()

convert_to_abs_paths(JZSerialPort_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSerialPort_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")