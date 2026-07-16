cmake_minimum_required(VERSION 3.10)
project(JZFramework)

set(CMAKE_CXX_STANDARD 17)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

#set(LVGL_ROOT_DIR "F:/mcu/jz_app/managed_components/lvgl__lvgl")

#set(LV_BUILD_CONF_DIR "${CMAKE_SOURCE_DIR}/test" CACHE PATH "")
#set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "")
#set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "")
#set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "")
#set(LV_BUILD_LVGL_H_SIMPLE_INCLUDE ON CACHE BOOL "")

#add_subdirectory(${LVGL_ROOT_DIR} ${CMAKE_BINARY_DIR}/lvgl)

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZNet JZBle JZLvgl)
target_link_libraries(JZFramework PRIVATE lvgl)
jzframework_build_test()
