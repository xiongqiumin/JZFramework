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
    JZMouseEvent.h
    JZMouseEvent.cpp
    JZImGuiBackend.h
    JZImGuiBackend.cpp
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
    JZWindow.h
    JZWindow.cpp
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

# ImGui 绘图后端 (FetchContent 拉取, 仅作为底层 2D 绘图器)
include("${JZFRAMEWORK_ROOT_DIR}/cmake/imgui.cmake")

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_SOURCES})

target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

if(WIN32)
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE imgui)
else()
    # 非 Windows 平台暂无渲染后端, 控件树逻辑层可编译运行
    message(STATUS "[JZWidget] No render backend on this platform (logic only)")
endif()
