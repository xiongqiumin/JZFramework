set(JZMODBUS_SOURCES
    JZModbusDefine.h
    JZModbusDefine.cpp
    JZModbusContext.h
    JZModbusContext.cpp
    JZModbusRtu.h
    JZModbusRtu.cpp
    JZModbusTcp.h
    JZModbusTcp.cpp
    JZModbusParam.h
    JZModbusParam.cpp
    JZModbusClient.h
    JZModbusClient.cpp
    JZModbusMaster.h
    JZModbusMaster.cpp
    JZModbusServer.h
    JZModbusServer.cpp
    JZModbusSlaver.h
    JZModbusSlaver.cpp
)

convert_to_abs_paths(JZMODBUS_SOURCES)

target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZMODBUS_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# 依赖 JZNet (JZSerialPort 在 JZNet 内)
target_include_directories(${JZ_MODULE_TARGET} PUBLIC
    "${JZFRAMEWORK_ROOT_DIR}/JZNet"
    "${JZFRAMEWORK_ROOT_DIR}/JZSerialPort"
    "${JZFRAMEWORK_ROOT_DIR}/JZCore"
)
