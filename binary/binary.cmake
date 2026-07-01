
#jzfrpc
set(JZFRPC_SOURCES jzfrpc.cpp)
convert_to_abs_paths(JZFRPC_SOURCES)

add_executable(jzfrpc ${JZFRPC_SOURCES})
target_link_libraries(jzfrpc JZFramework)

#jzfrps
set(JZFRPS_SOURCES jzfrps.cpp)
convert_to_abs_paths(JZFRPS_SOURCES)

add_executable(jzfrps ${JZFRPS_SOURCES})
target_link_libraries(jzfrps JZFramework)