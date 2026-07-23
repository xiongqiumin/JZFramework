set(JZOTA_SOURCES
    JZOTA.h
    JZOTA.cpp
    JZOTADriver.h
    JZOTADriver.cpp
)

# 平台相关驱动，仅编译对应平台的实现
if(ESP_PLATFORM)
    list(APPEND JZOTA_SOURCES
        JZOTADriverEsp32.h
        JZOTADriverEsp32.cpp
    )
endif()

convert_to_abs_paths(JZOTA_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZOTA_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
