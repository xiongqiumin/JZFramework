#test_lvgl
set(TEST_LVGL_SOURCES test_lvgl.cpp)
convert_to_abs_paths(TEST_LVGL_SOURCES)

add_executable(test_lvgl ${TEST_LVGL_SOURCES})
target_link_libraries(test_lvgl JZFramework lvgl)

#test_mainwindow
set(TEST_ESP_SOURCES test_esp.cpp main_window.cpp main_window.h)
convert_to_abs_paths(TEST_ESP_SOURCES)

add_executable(test_esp ${TEST_ESP_SOURCES})
target_link_libraries(test_esp JZFramework lvgl)

# test_ble_periph — BLE Peripheral 上行流程测试 (纯控制台, 使用 JZBleDriverSimulator)
set(TEST_BLE_PERIPH_SOURCES test_ble_periph.cpp)
convert_to_abs_paths(TEST_BLE_PERIPH_SOURCES)

add_executable(test_ble_periph ${TEST_BLE_PERIPH_SOURCES})
target_link_libraries(test_ble_periph JZFramework)