set(JZHTTP_SERVER_SOURCES    
    JZHttpRequest.cpp
	JZHttpRequest.h
    JZHttpRoute.cpp
	JZHttpRoute.h
    JZHttpRespone.cpp
	JZHttpRespone.h
	JZSendFromDirectory.h
	JZSendFromDirectory.cpp
    JZHttpServer.cpp
	JZHttpServer.h
)
convert_to_abs_paths(JZHTTP_SERVER_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZHTTP_SERVER_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")