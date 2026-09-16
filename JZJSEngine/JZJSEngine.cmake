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
	JZJSWorkerThread.h
	JZJSWorkerThread.cpp
	JZJSWorkerModule.h
	JZJSWorkerModule.cpp
	JZJSSerialPortModule.h
	JZJSSerialPortModule.cpp
	JZJSCoreModule.h
	JZJSCoreModule.cpp
	JZJSqliteModule.h
	JZJSqliteModule.cpp
	JZJSModbusModule.h
	JZJSModbusModule.cpp
	JZJSUiLoader.h
	JZJSUiLoader.cpp
)

# JS 侧的界面能力建立在自绘 UI(JZWidget)之上: 没启用 JZWidget 时不参与编译,
# C++ 侧对应的 #ifdef HAS_JZWidget 守卫见 JZJSEngine.cpp 的模块注册处。
if(HAS_JZWidget)
    list(APPEND JZJSENGINE_SOURCES JSWidget/JZJSWidgetModule.h
		JSWidget/JZJSWidgetModule.cpp
		JSWidget/JZJSWidget.h
		JSWidget/JZJSWidget.cpp)
endif()

convert_to_abs_paths(JZJSENGINE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZJSENGINE_SOURCES})
target_compile_definitions(${JZ_MODULE_TARGET} PRIVATE __STDC_NO_ATOMICS__)
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/JSWidget")