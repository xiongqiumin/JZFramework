#test_lvgl
set(TEST_LVGL_SOURCES test_lvgl.cpp)
convert_to_abs_paths(TEST_LVGL_SOURCES)

add_executable(test_lvgl ${TEST_LVGL_SOURCES})
target_link_libraries(test_lvgl JZFramework lvgl)
target_link_directories(test_lvgl PRIVATE "${JZFRAMEWORK_ROOT_DIR}")
