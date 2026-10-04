# 工具集: 复用主树 JZFramework 静态库, 不再各自独立构建
add_subdirectory(monitor)

# .ui -> C++ 代码生成器(构建期工具; 由 JZFramework.cmake 的 jz_add_ui 调用)
add_subdirectory(jz_uic)
