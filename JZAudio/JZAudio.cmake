set(JZMultimedia_SOURCES    
    JZAudioOutput.h
    JZAudioOutput.cpp
    JZAudioInput.h
    JZAudioInput.cpp
    JZAudioFormat.h
    JZAudioFormat.cpp
	JZAudioDevice.h
    JZAudioDevice.cpp
	JZAudioUtils.h
	JZAudioUtils.cpp
	JZWaveFile.h
	JZWaveFile.cpp
)

if(WIN32)
    # Windows平台：添加Windows特有的音频实现文件
    list(APPEND JZMultimedia_SOURCES
        windows/JZAudioInputImpl.cpp
		windows/JZAudioInputImpl.h
		windows/JZAudioOutputImpl.cpp
		windows/JZAudioOutputImpl.h
        windows/JZAudioDeviceImpl.cpp
		windows/JZAudioDeviceImpl.h
    )
	
	target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/windows")
	target_link_libraries(${JZ_MODULE_TARGET} PRIVATE winmm)

elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux平台：添加Linux特有的音频实现文件
    list(APPEND JZMultimedia_SOURCES
        linux/JZAudioInputImpl.cpp
        linux/JZAudioInputImpl.h
        linux/JZAudioOutputImpl.cpp
        linux/JZAudioOutputImpl.h
        linux/JZAudioDeviceImpl.cpp
		linux/JZAudioDeviceImpl.h
    )
	
    target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/linux")
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE asound)
endif()

convert_to_abs_paths(JZMultimedia_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZMultimedia_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")