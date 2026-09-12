set(JZXML_SOURCES
    JZXml.h
    JZXml.cpp
)

convert_to_abs_paths(JZXML_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZXML_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
