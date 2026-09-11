set(JZ_SQLITE_DIR "${CMAKE_CURRENT_LIST_DIR}")

set(JZDATABASE_SOURCES
    "sqlite3.c"
    "sqlite3.h"
    "JZKvDatabase.cpp"
    "JZKvDatabase.h"
)
convert_to_abs_paths(JZDATABASE_SOURCES)

# sqlite3.c 是 9MB 的 amalgamation 单文件, 开着 /W3 会刷出成百上千条无关警告;
# 只对这一个文件静默(不修改其源码, 也不影响框架其它文件的警告级别)。
if(MSVC)
    set_source_files_properties("${JZ_SQLITE_DIR}/sqlite3.c"
        PROPERTIES COMPILE_OPTIONS "/w")
else()
    set_source_files_properties("${JZ_SQLITE_DIR}/sqlite3.c"
        PROPERTIES COMPILE_OPTIONS "-w")
endif()

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZDATABASE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${JZ_SQLITE_DIR}")
