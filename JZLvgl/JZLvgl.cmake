set(JZLvgl_SOURCES
    JZLvgl.h
    JZKeyEvent.h
    JZPen.h
    JZBrush.h
    JZFont.h
    JZPainter.h
    JZPainter.cpp
    JZLvglLayout.h
    JZLvglLayout.cpp
    JZAbstractAnimation.h
    JZAbstractAnimation.cpp
    JZPropertyAnimation.h
    JZPropertyAnimation.cpp
    JZAnimationGroup.h
    JZAnimationGroup.cpp
    JZLvglWidget.h
    JZLvglWidget.cpp
    JZLvglButton.h
    JZLvglButton.cpp
    JZLvglLabel.h
    JZLvglLabel.cpp
    JZImage.h
    JZLvglImage.h
    JZLvglImage.cpp
    JZLvglWindow.h
    JZLvglWindow.cpp
    JZScreen.h
    JZScreen.cpp
    JZLvglSlider.h
    JZLvglSlider.cpp
    JZLvglBar.h
    JZLvglBar.cpp
    JZLvglSwitch.h
    JZLvglSwitch.cpp
    JZLvglCheckbox.h
    JZLvglCheckbox.cpp
    JZLvglDropdown.h
    JZLvglDropdown.cpp
    JZLvglArc.h
    JZLvglArc.cpp
    JZLvglTextarea.h
    JZLvglTextarea.cpp
    JZLvglLed.h
    JZLvglLed.cpp
    JZLvglRoller.h
    JZLvglRoller.cpp
    JZLvglTable.h
    JZLvglTable.cpp
)

convert_to_abs_paths(JZLvgl_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZLvgl_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# LVGL 依赖（由项目 CMakeLists.txt add_subdirectory 引入 lvgl target）
if(TARGET lvgl)
    target_link_libraries(${JZ_MODULE_TARGET} PUBLIC lvgl)
endif()
