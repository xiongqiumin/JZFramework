set(JZAGENT_SOURCES
	JZAgent.h
    JZAgent.cpp
	JZAgentTool.h
	JZAgentTool.cpp
	JZMcpClient.h
	JZMcpClient.cpp
	JZLoopDetect.h
	JZLoopDetect.cpp
)

convert_to_abs_paths(JZAGENT_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZAGENT_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")