set(JZCamera_SOURCES
    JZCamera.h
    JZCamera.cpp
    JZCameraFrame.h
    JZCameraFrame.cpp
    JZCameraDriver.h
    JZCameraDriver.cpp
)

# ESP32 驱动: 仅在 ESP-IDF 环境下编译(esp_camera.h 仅 ESP32 提供)
if(DEFINED ESP_PLATFORM OR DEFINED CONFIG_IDF_TARGET)
    list(APPEND JZCamera_SOURCES
        JZCameraDriverEsp32.h
        JZCameraDriverEsp32.cpp
    )
endif()

convert_to_abs_paths(JZCamera_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCamera_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
