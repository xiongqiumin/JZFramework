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
	JZWaveFile.h
	JZWaveFile.cpp
    detail/JZAudioDriver.h
    detail/JZAudioDriver.cpp
    detail/JZAudioInputImpl.h
    detail/JZAudioInputImpl.cpp
    detail/JZAudioOutputImpl.h
    detail/JZAudioOutputImpl.cpp
)

if(WIN32)
    # Windows平台：添加Windows特有的音频实现文件
    list(APPEND JZAudio_SOURCES
        detail/windows/JZAudioInputWindows.cpp
		detail/windows/JZAudioInputWindows.h
		detail/windows/JZAudioOutputWindows.cpp
		detail/windows/JZAudioOutputWindows.h
        detail/windows/JZAudioDriverWindows.cpp
		detail/windows/JZAudioDriverWindows.h
    )
	
	target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/windows")
	target_link_libraries(${JZ_MODULE_TARGET} PRIVATE winmm)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux平台：添加Linux特有的音频实现文件
    list(APPEND JZAudio_SOURCES
        detail/linux/JZAudioInputLinux.cpp
        detail/linux/JZAudioInputLinux.h
        detail/linux/JZAudioOutputLinux.cpp
        detail/linux/JZAudioOutputLinux.h
        detail/linux/JZAudioDriverLinux.cpp
		detail/linux/JZAudioDriverLinux.h
    )
	
    target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/linux")
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE asound)    
endif()

convert_to_abs_paths(JZAudio_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZAudio_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")