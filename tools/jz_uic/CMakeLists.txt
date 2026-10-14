# jz_uic —— .ui -> C++ 代码生成器(构建期工具, 由 CMake 函数 jz_add_ui 调用)
# 用 C++ 写(而不是 Python)是有意的: 方言表/挂载规则/属性白名单全在 JZUiParser 里,
# 复用它才能保证"运行期 loader / JS 生成器 / C++ 生成器"三个后端对同一份 .ui 理解一致。
add_executable(jz_uic main.cpp)
target_link_libraries(jz_uic JZFramework)

# 输出目录: 非发布 exe 统一到 <build>/test/bin/<配置>(根 CMakeLists 的 JZ_TEST_BIN_DIR)。
# 构建期调用方用 $<TARGET_FILE:jz_uic>(见 JZFramework.cmake 的 jz_add_ui), 跟着走不会断。
set_target_properties(jz_uic PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${JZ_TEST_BIN_DIR}/$<CONFIG>")
