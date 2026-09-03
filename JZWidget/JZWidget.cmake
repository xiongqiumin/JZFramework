# JZWidget 目前仅支持 Windows 平台 (GDI+ 渲染后端 / 原生窗口)。
# 非 Windows 平台暂不编译此模块。
if(NOT WIN32)
    return()
endif()

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
    JZImeEvent.h
    JZImeEvent.cpp
    JZPen.h
    JZPen.cpp
    JZBrush.h
    JZBrush.cpp
    JZFont.h
    JZFont.cpp
    JZFontMetrics.h
    JZFontMetrics.cpp
    JZStyleSet.h
    JZStylePainter.h
    JZStylePainter.cpp
    JZPainter.h
    JZPainter.cpp
    JZPainterEvent.h
    JZPainterEvent.cpp
    JZResizeEvent.h
    JZResizeEvent.cpp
    JZShowEvent.h
    JZShowEvent.cpp
	JZHideEvent.h
	JZHideEvent.cpp
    JZCloseEvent.h
    JZCloseEvent.cpp
    JZMouseEvent.h
    JZMouseEvent.cpp
    JZGdiBackend.h
    JZGdiBackend.cpp
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
    JZTabWidget.h
    JZTabWidget.cpp
    JZAiDriver.h
    JZAiDriver.cpp
    JZScrollArea.h
    JZScrollArea.cpp
    JZListWidget.h
    JZListWidget.cpp
    JZSlider.h
    JZSlider.cpp
    JZSpinBox.h
    JZSpinBox.cpp
    JZScrollBar.h
    JZScrollBar.cpp
    JZGroupBox.h
    JZGroupBox.cpp
    JZSplitter.h
    JZSplitter.cpp
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
    JZLineEdit.h
    JZLineEdit.cpp
    JZTextEdit.h
    JZTextEdit.cpp    
    JZTableWidget.h
	JZTableWidget.cpp
    JZTreeWidget.h
    JZTreeWidget.cpp
    JZDesktopServices.h
    JZDesktopServices.cpp
)

convert_to_abs_paths(JZ_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_SOURCES})

target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# ---- AI/MCP 工具桥 (可选): JZMcpServer 模块参与构建时, 把语义树/截图/驱动
#      暴露为 MCP 工具 (get_ui_state / ui_screenshot / ui_invoke)。
#      注意: JZMcpServer 需在 jzframework_use_modules 中排在 JZWidget 之前。
jz_module_exists(JZMcpServer JZ_HAS_MCP_SERVER)
if(JZ_HAS_MCP_SERVER)
    set(JZ_AI_SOURCES
        JZUiMcpTools.h
        JZUiMcpTools.cpp
    )
    convert_to_abs_paths(JZ_AI_SOURCES)
    target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_AI_SOURCES})
    message(STATUS "[JZWidget] JZMcpServer enabled -> AI MCP tools (JZUiMcpBridge) added")
endif()

if(WIN32)
    # GDI+ 绘图后端 (临时方案; 后续如需 GPU 渲染, 按同样接口接入新后端)
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE gdiplus imm32 shell32)
else()
    # 非 Windows 平台暂无渲染后端, 控件树逻辑层可编译运行
    message(STATUS "[JZWidget] No render backend on this platform (logic only)")
endif()
