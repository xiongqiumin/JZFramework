set(JZBLE_SOURCES    
    JZBle.cpp
	JZBle.h
)
convert_to_abs_paths(JZBLE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZBLE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")