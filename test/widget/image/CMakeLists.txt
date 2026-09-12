# 目标名不能叫 test_image: test/agent 下已有同名可执行目标 (CMP0002 全局唯一)。
add_executable(test_imageio test_image.cpp)
target_link_libraries(test_imageio JZFramework)

# JZImage -> JZPixmap (显示用位图) 的纯 CPU 转换校验, 不需要窗口, 可自动跑。
add_executable(test_pixmap test_pixmap.cpp)
target_link_libraries(test_pixmap JZFramework)
add_test(NAME widget.test_pixmap COMMAND test_pixmap)

# JZPrinter: 离屏 JZPainter 绘制 -> 多页 PDF 导出
add_executable(test_jzprinter test_jzprinter.cpp)
target_link_libraries(test_jzprinter JZFramework)
add_test(NAME widget.test_jzprinter COMMAND test_jzprinter)
