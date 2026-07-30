set(JZ_SOURCES
    JZColor.h
    JZColor.cpp
    JZDrawTask.h
    JZDrawTask.cpp
    JZLayer.h
    JZLayer.cpp
    JZSize.h
    JZSize.cpp
    JZPoint.h
    JZPoint.cpp
    JZRect.h
    JZRect.cpp
    JZKeyEvent.h
    JZKeyEvent.cpp
    JZPen.h
    JZPen.cpp
    JZBrush.h
    JZBrush.cpp
    JZFont.h
    JZFont.cpp
    JZPainter.h
    JZPainter.cpp
    JZPainterEvent.h
    JZPainterEvent.cpp
    JZResizeEvent.h
    JZResizeEvent.cpp
    JZShowEvent.h
    JZShowEvent.cpp
    JZCloseEvent.h
    JZCloseEvent.cpp
    JZLayout.h
    JZLayout.cpp
    JZAbstractAnimation.h
    JZAbstractAnimation.cpp
    JZPropertyAnimation.h
    JZPropertyAnimation.cpp
    JZAnimationGroup.h
    JZAnimationGroup.cpp
    JZWidget.h
    JZWidget.cpp
    JZPushButton.h
    JZPushButton.cpp
    JZLabel.h
    JZLabel.cpp
	JZImageLabel.h
	JZImageLabel.cpp
    JZImage.h
    JZImage.cpp
    JZMainWindow.h
    JZMainWindow.cpp
    JZDialog.h
    JZDialog.cpp
    JZMessageBox.h
    JZMessageBox.cpp
    JZDisplayDevice.h
    JZDisplayDevice.cpp
    JZStackedWidget.h
    JZStackedWidget.cpp
    JZScrollArea.h
    JZScrollArea.cpp
    JZListWidget.h
    JZListWidget.cpp
    JZSlider.h
    JZSlider.cpp
    JZProgressBar.h
    JZProgressBar.cpp
    JZSwitch.h
    JZSwitch.cpp
    JZCheckbox.h
    JZCheckbox.cpp
    JZRadioButton.h
    JZRadioButton.cpp
    JZComboBox.h
    JZComboBox.cpp    
    JZTextEdit.h
    JZTextEdit.cpp    
	JZTableWidget.h
	JZTableWidget.cpp
)

convert_to_abs_paths(JZ_SOURCES)

if(WIN32)
    set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/lvgl/lv_conf_windows.h CACHE FILEPATH "LVGL config" FORCE)
else()
    set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_LIST_DIR}/lvgl/lv_conf_linux.h CACHE FILEPATH "LVGL config" FORCE)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/lvgl/lvgl.cmake)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_SOURCES})


target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/lvgl")
if(NOT WIN32)
    find_package(X11 REQUIRED)
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE X11::X11)
endif()

target_link_libraries(${JZ_MODULE_TARGET} PRIVATE lvgl)
