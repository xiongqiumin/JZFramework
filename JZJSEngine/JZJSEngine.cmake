set(JZJSENGINE_SOURCES
    quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/dtoa.c
    quickjs/quickjs.c

	JZJSType.h
	JZJSType.cpp
	JZJSBind.h
	JZJSBind.cpp
	JZJSEngine.h
	JZJSEngine.cpp
	JZJSHttpModule.h
	JZJSHttpModule.cpp
)

# quickjs-ng 0.15.1 的 quickjs-c-atomics.h 使用 C11 <stdatomic.h>,
# MSVC 默认按 C89 编译 .c 会报 "C atomics require C11 or later", 故对 C 源文件启用 C11。
# 仅作用于 C 语言文件 (COMPILE_LANGUAGE:C), 不影响 C++ 文件。
target_compile_options(${JZ_MODULE_TARGET} PRIVATE "$<$<COMPILE_LANGUAGE:C>:/std:c11>")

convert_to_abs_paths(JZJSENGINE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZJSENGINE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")