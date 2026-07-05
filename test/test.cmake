#test_lvgl
set(JZFRPC_SOURCES test_core.cpp)
convert_to_abs_paths(JZFRPC_SOURCES)

add_executable(test_lvgl ${JZFRPC_SOURCES})
target_link_libraries(test_lvgl JZFramework)
