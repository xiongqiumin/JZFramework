set(JZTEST_SOURCES    
    JZTest.cpp
	JZTest.h
	JZTestMacro.h
	JZTestSuite.cpp
	JZTestSuite.h
	JZBenchmark.cpp
	JZBenchmark.h
)
convert_to_abs_paths(JZTEST_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZTEST_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")