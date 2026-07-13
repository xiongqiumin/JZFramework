set(JZSIMULATOR_SOURCES
    JZSimPack.h
    JZSimPack.cpp
	JZBleDriverSimulator.h
	JZBleDriverSimulator.cpp
	JZGpioDriverSimulator.h
	JZGpioDriverSimulator.cpp
	JZWifiDriverSimulator.h
	JZWifiDriverSimulator.cpp
)
convert_to_abs_paths(JZSIMULATOR_SOURCES)

target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZSIMULATOR_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")