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
	JZJSNetModule.h
	JZJSNetModule.cpp
	JZJSCoreModule.h
	JZJSCoreModule.cpp
)

convert_to_abs_paths(JZJSENGINE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZJSENGINE_SOURCES})
target_compile_definitions(${JZ_MODULE_TARGET} PRIVATE __STDC_NO_ATOMICS__)
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")