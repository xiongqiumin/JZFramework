set(JZJSON_SOURCES
	JZJson.h
	JZJson.cpp
	JZJsonStream.h
	JZJsonStream.cpp
	json/src/lib_json/json_value.cpp
	json/src/lib_json/json_writer.cpp
	json/src/lib_json/json_reader.cpp
)

convert_to_abs_paths(JZJSON_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZJSON_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/json/include")