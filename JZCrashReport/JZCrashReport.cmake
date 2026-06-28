set(JZCrashReport_SOURCES    
    JZCrashReport.h
    JZCrashReport.cpp
)

if(WIN32)
    list(APPEND JZCrashReport_SOURCES
        JZCrashReportWindows.cpp
    )
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND JZCrashReport_SOURCES
        JZCrashReportLinux.cpp
    )
endif()

convert_to_abs_paths(JZCrashReport_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCrashReport_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")