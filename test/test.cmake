#test_lvgl
set(TEST_LVGL_SOURCES test_lvgl.cpp)
convert_to_abs_paths(TEST_LVGL_SOURCES)

add_executable(test_lvgl ${TEST_LVGL_SOURCES})
target_link_libraries(test_lvgl JZFramework lvgl)
target_link_directories(test_lvgl PRIVATE "${JZFRAMEWORK_ROOT_DIR}")

#test_wifi
set(TEST_WIFI_SOURCES test_wifi.cpp)
convert_to_abs_paths(TEST_WIFI_SOURCES)

add_executable(test_wifi ${TEST_WIFI_SOURCES})
target_link_libraries(test_wifi JZFramework)
target_link_directories(test_wifi PRIVATE "${JZFRAMEWORK_ROOT_DIR}")

#test_wifiAuto
set(TEST_WIFIAUTO_SOURCES test_wifiAuto.cpp)
convert_to_abs_paths(TEST_WIFIAUTO_SOURCES)

add_executable(test_wifiAuto ${TEST_WIFIAUTO_SOURCES})
target_link_libraries(test_wifiAuto JZFramework)
target_link_directories(test_wifiAuto PRIVATE "${JZFRAMEWORK_ROOT_DIR}")
