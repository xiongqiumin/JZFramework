set(JZCO_SOURCES    
    JZCo.cpp
	JZCo.h
	JZCoContext.cpp
	JZCoContext.h
	JZCoCoroutine.cpp
	JZCoCoroutine.h
	JZCoFile.cpp
	JZCoFile.h
	JZCoMutex.cpp
	JZCoMutex.h
    JZCoSemaphore.cpp
    JZCoSemaphore.h
    JZCoWatch.cpp
    JZCoWatch.h
)

set(JZCO_NET_SOURCES
	JZCoNetworkAccessManager.cpp
    JZCoNetworkAccessManager.h
    JZCoTcpSocket.cpp
	JZCoTcpSocket.h
    JZCoTcpServer.cpp
    JZCoTcpServer.h
    JZCoUdpSocket.cpp
    JZCoUdpSocket.h
    JZCoWebSocket.cpp
	JZCoWebSocket.h
    JZCoWebSocketServer.cpp
	JZCoWebSocketServer.h
)

if(HAS_JZNet)
    list(APPEND JZCO_SOURCES ${JZCO_NET_SOURCES})
endif()

set(JZCO_BLE_SOURCES
    JZCoBleCentral.cpp
    JZCoBleCentral.h
    JZCoBleService.cpp
    JZCoBleService.h
    JZCoBleDiscoveryAgent.cpp
    JZCoBleDiscoveryAgent.h
)

if(HAS_JZBle)
    list(APPEND JZCO_SOURCES ${JZCO_BLE_SOURCES})
endif()

set(JZCO_SERIAL_SOURCES
	JZCoSerialPort.cpp
    JZCoSerialPort.h
)

if(HAS_JZSerialPort)
    list(APPEND JZCO_SOURCES ${JZCO_SERIAL_SOURCES})
endif()

# JZCoProcess only on platforms where JZProcess is available (Win/Linux)
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND JZCO_SOURCES JZCoProcess.cpp JZCoProcess.h)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")

# 自动判断架构
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64")
    set(BOOST_CONTEXT_ARCHITECTURE "x86_64")
    set(BOOST_CONTEXT_ABI "sysv")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(BOOST_CONTEXT_ARCHITECTURE "arm64")
    set(BOOST_CONTEXT_ABI "aapcs")
else()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 二进制格式 ELF / PE / Mach-O
if(APPLE)
    set(BOOST_CONTEXT_BINARY_FORMAT "macho")
elseif(UNIX)
    set(BOOST_CONTEXT_BINARY_FORMAT "elf")
elseif(WIN32)
    set(BOOST_CONTEXT_BINARY_FORMAT "pe")
else()
    message(FATAL_ERROR "Unsupported OS")
endif()

# 汇编器
set(BOOST_CONTEXT_ASSEMBLER "gas")
set(BOOST_CONTEXT_ASM_SUFFIX ".S")

# 拼接最终汇编文件名后缀
string(CONCAT ASM_SUFFIX
    "${BOOST_CONTEXT_ARCHITECTURE}_"
    "${BOOST_CONTEXT_ABI}_"
    "${BOOST_CONTEXT_BINARY_FORMAT}_"
    "${BOOST_CONTEXT_ASSEMBLER}"
    "${BOOST_CONTEXT_ASM_SUFFIX}"
)

set(_asm_suffix ${BOOST_CONTEXT_ARCHITECTURE}_${BOOST_CONTEXT_ABI}_${BOOST_CONTEXT_BINARY_FORMAT}_${BOOST_CONTEXT_ASSEMBLER}${BOOST_CONTEXT_ASM_SUFFIX})

set(ASM_SOURCES
	boost_context/asm/make_${_asm_suffix}
	boost_context/asm/jump_${_asm_suffix}
	boost_context/asm/ontop_${_asm_suffix}
)

list(APPEND JZCO_SOURCES
        boost_context/fcontext.cpp
		${ASM_SOURCES}
    )

endif()

convert_to_abs_paths(JZCO_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCO_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")