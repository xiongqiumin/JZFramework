set(JZ_SOURCES
    JZMcpCore.h
    JZMcpCore.cpp
    JZMcpTool.h
    JZMcpTool.cpp
    JZMcpServer.h
    JZMcpServer.cpp
)

convert_to_abs_paths(JZ_SOURCES)

target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZ_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
