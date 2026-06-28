set(JZNODE_SOURCES
    JZNode.h
    JZNode.cpp
	JZNodeManager.h
	JZNodeManager.cpp
)
convert_to_abs_paths(JZNODE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZNODE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")