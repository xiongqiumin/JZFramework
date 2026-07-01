set(JZAudio_SOURCES    
    JZAudioOutput.h
    JZAudioOutput.cpp
    JZAudioInput.h
    JZAudioInput.cpp
    JZAudioFormat.h
    JZAudioFormat.cpp
	JZAudioDeviceInfo.h
    JZAudioDeviceInfo.cpp
	JZAudioUtils.h
	JZAudioUtils.cpp
    JZAudioDriver.h
    JZAudioDriver.cpp
	JZWaveFile.h
	JZWaveFile.cpp
)

if(WIN32)
    # Windows平台：添加Windows特有的音频实现文件
    list(APPEND JZAudio_SOURCES
        windows/JZAudioInputImpl.cpp
		windows/JZAudioInputImpl.h
		windows/JZAudioOutputImpl.cpp
		windows/JZAudioOutputImpl.h
        windows/JZAudioDriverLinux.cpp
		windows/JZAudioDriverLinux.h
    )
	
	target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/windows")
	target_link_libraries(${JZ_MODULE_TARGET} PRIVATE winmm)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux平台：添加Linux特有的音频实现文件
    list(APPEND JZAudio_SOURCES
        linux/JZAudioInputImpl.cpp
        linux/JZAudioInputImpl.h
        linux/JZAudioOutputImpl.cpp
        linux/JZAudioOutputImpl.h
        linux/JZAudioDriverWin32.cpp
		linux/JZAudioDriverWin32.h
    )
	
    target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/linux")
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE asound)

else()
    list(APPEND JZAudio_SOURCES
        rtos/JZAudioInputImpl.cpp
        rtos/JZAudioInputImpl.h
        rtos/JZAudioOutputImpl.cpp
        rtos/JZAudioOutputImpl.h
    )

    target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/rtos")
endif()

convert_to_abs_paths(JZAudio_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZAudio_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")