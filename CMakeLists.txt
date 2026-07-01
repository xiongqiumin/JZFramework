cmake_minimum_required(VERSION 3.10)
project(JZFramework)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

# 可选：引入 LVGL
if(DEFINED JZ_LVGL_ROOT)
    set(LV_BUILD_CONF_DIR "${JZ_LVGL_CONFIG_DIR}" CACHE PATH "")
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "")
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "")
    set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "")
    add_subdirectory(${JZ_LVGL_ROOT} ${CMAKE_BINARY_DIR}/lvgl)
endif()

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZLvgl)
jzframework_build_test()
