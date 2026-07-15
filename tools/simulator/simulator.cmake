set(JZSIMULATOR_SOURCES
    JZSimulator.h
    JZSimulator.cpp
    main.cpp
)
convert_to_abs_paths(JZSIMULATOR_SOURCES)

# 模拟器设备可执行程序(带 LVGL 界面)
add_executable(JZSimulator ${JZSIMULATOR_SOURCES})
target_link_libraries(JZSimulator JZFramework lvgl)
