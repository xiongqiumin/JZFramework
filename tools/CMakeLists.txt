# 工具集: 复用主树 JZFramework 静态库, 不再各自独立构建
add_subdirectory(monitor)

# .ui -> C++ 代码生成器(构建期工具; 由 JZFramework.cmake 的 jz_add_ui 调用)
#
# 依赖 JZWidget 的 JZUiLoader.h / JZUiCppGen.h, 而 JZWidget 仅 Windows 可用
# (见 JZWidget/JZWidget.cmake 开头的 `if(NOT WIN32) return()`)。
# 非 Windows 平台上 HAS_JZWidget 为假, 此时不建本工具 —— 与 test/ 的 `if(WIN32)
# add_subdirectory(widget)` 和 binary/ 的 `if(HAS_JZWidget)` 保持一致口径。
# 注: jz_add_ui() 的唯一调用方(test/widget/bteserver)同样在 WIN32/有 JZWidget 时
#     才存在, 故跳过后不会触发 JZFramework.cmake 里的 "需要 jz_uic 目标" 报错。
if(HAS_JZWidget)
    add_subdirectory(jz_uic)
endif()
