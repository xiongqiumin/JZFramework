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
    JZStyleSet.cpp
    JZStyleSheetParser.h
    JZStyleSheetParser.cpp
    JZStylePainter.h
    JZStylePainter.cpp
    JZPainter.h
    JZPainter.cpp
    JZPrinter.h
    JZPrinter.cpp
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
    JZRenderBackend.h
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
    JZPixmap.h
    JZPixmap.cpp
    JZImageReader.h
    JZImageReader.cpp
    JZImageWriter.h
    JZImageWriter.cpp
    JZStbImage.cpp
    JZMainWindow.h
    JZMainWindow.cpp
    JZWindow.h
    JZWindow.cpp    
    JZChart.h
    JZChart.cpp
    JZDialog.h
    JZDialog.cpp
    JZMessageBox.h
    JZMessageBox.cpp
    JZDisplayDevice.h
    JZDisplayDevice.cpp
    JZVirtualDevice.h
    JZVirtualDevice.cpp
    JZScreen.h
    JZScreen.cpp
    JZStackedWidget.h
    JZStackedWidget.cpp
    JZTabWidget.h
    JZTabWidget.cpp
    JZAiDriver.h
    JZAiDriver.cpp
    JZScrollArea.h
    JZScrollArea.cpp
    JZAbstractViewWidget.h
    JZAbstractViewWidget.cpp
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
    JZUndoStack.h
    JZUndoStack.cpp
    JZTextImageMap.h
    JZTextImageMap.cpp
    JZEmoji.h
    JZEmoji.cpp
    JZEmojiImageMap.h
    JZEmojiImageMap.cpp
    JZTextEmoji.h
    JZTextEmoji.cpp
    JZAbstractTextEdit.h
    JZAbstractTextEdit.cpp
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
    JZAction.h
    JZAction.cpp
    JZMenu.h
    JZMenu.cpp
    JZMenuBar.h
    JZMenuBar.cpp
    JZFileDialog.h
    JZFileDialog.cpp
)

convert_to_abs_paths(JZ_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_SOURCES})

target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# 内置 emoji 位图资源 (Twemoji 72x72 PNG, ~230KB): assets/emoji/emoji.qrc
# -> 静态库 JZWidget_emoji_resources (暴露 jzres_emoji_init())。
# JZEmojiImageMap 首次查图时会懒调用该 init, 使用方无需手动初始化。
# 注意: 必须用 PUBLIC 传递, 否则链接 JZFramework 的可执行文件不会把该静态库
#       拉进来, jzres_emoji_init() 会变成未定义(运行期查不到资源)。
jz_add_resources(JZWidget_emoji_resources ${CMAKE_CURRENT_LIST_DIR}/assets/emoji/emoji.qrc)
target_link_libraries(${JZ_MODULE_TARGET} PUBLIC JZWidget_emoji_resources)

# ---- AI/MCP 工具桥 (可选): JZMcpServer 模块参与构建时, 把语义树/截图/驱动
#      暴露为 MCP 工具 (get_ui_state / ui_screenshot / ui_invoke)。
#      与模块顺序无关: HAS_JZMcpServer 是 jzframework_use_modules 开头就定好的全局缓存变量
#      (见 JZFramework.cmake "模块启用状态")。
if(HAS_JZMcpServer)
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
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE gdiplus imm32 shell32 comdlg32 ole32)
else()
    # 非 Windows 平台暂无渲染后端, 控件树逻辑层可编译运行
    message(STATUS "[JZWidget] No render backend on this platform (logic only)")
endif()
