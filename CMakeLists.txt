cmake_minimum_required(VERSION 3.10)
project(JZFramework)

set(CMAKE_CXX_STANDARD 17)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

# LVGL源码路径，空表示禁用
set(LVGL_ROOT_PATH "" CACHE PATH "Path to LVGL source root. Leave empty to disable.")

if(LVGL_ROOT_PATH)
    if(NOT IS_DIRECTORY ${LVGL_ROOT_PATH})
        message(FATAL_ERROR "LVGL_ROOT_PATH '${LVGL_ROOT_PATH}' is not a valid directory.")
    endif()

    set(LV_BUILD_CONF_DIR "${CMAKE_SOURCE_DIR}/test" CACHE PATH "")
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "")
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "")
    set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "")
    set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "")

    message(STATUS "LVGL enabled, source: ${LVGL_ROOT_PATH}")
    add_subdirectory(${LVGL_ROOT_PATH} ${CMAKE_BINARY_DIR}/lvgl)
else()
    message(STATUS "LVGL disabled (LVGL_ROOT_PATH is empty)")
endif()

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZNet JZAgent JZBle JZCamera JZCoroutine JZCrashReport
    JZCv JZFrp JZGpio JZHttpServer JZJSEngine JZJson JZLvgl JZMcpServer JZModbus JZNode
    JZOTA JZSensor JZSerialPort JZWifi JZTest)

add_subdirectory(demo)
add_subdirectory(test)
add_subdirectory(tools)
add_subdirectory(binary)
