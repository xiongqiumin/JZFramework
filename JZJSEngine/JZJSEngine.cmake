set(JZJSENGINE_SOURCES
    quickjs/cutils.c
	quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/xsum.c
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

convert_to_abs_paths(JZJSENGINE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZJSENGINE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")