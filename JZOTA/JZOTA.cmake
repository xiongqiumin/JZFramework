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

    # OTA verify 当前仅用到分区查询接口, 最小依赖集
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE
        idf::app_update       # esp_ota_get_next_update_partition 等 OTA 接口
        idf::esp_partition     # esp_partition_t 分区信息
    )
    # 实现下载/写分区时再加: esp_https_ota esp_http_client esp-tls nvs_flash spi_flash
endif()

convert_to_abs_paths(JZOTA_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZOTA_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
