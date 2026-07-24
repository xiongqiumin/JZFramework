set(JZCORE_SOURCES
    JZGlobal.h
    JZGlobal.cpp
	JZString.h
	JZString.cpp
	JZRegExp.h
	JZRegExp.cpp
    JZAllocator.h
    JZByteArray.h
    JZByteArray.cpp    
    JZDataStream.h
    JZDataStream.cpp
	JZTextStream.h
	JZTextStream.cpp
    JZDateTime.h
    JZDateTime.cpp
    JZPlatform.h
    JZPlatform.cpp    
    JZStd.h
    JZStd.cpp
    JZThread.h
    JZThread.cpp
    JZDir.h
    JZDir.cpp
    JZFileInfo.h
    JZFileInfo.cpp
    JZFile.h
    JZFile.cpp
    JZResources.h
    JZResources.cpp
    JZLog.h
    JZLog.cpp
    JZTx.h
    JZTx.cpp
    JZEvent.h
    JZEvent.cpp
    JZSignal.h
    JZSignal.cpp
    JZObject.h
    JZObject.cpp
	JZObjectPointer.h
	JZObjectPointer.cpp
	JZApplication.h
    JZApplication.cpp
    JZDriver.h
    JZDriver.cpp
    JZEventLoop.h
    JZEventLoop.cpp
    JZSemaphore.h
    JZSemaphore.cpp
    JZMutex.h
    JZMutex.cpp
    JZCrypto.h
    JZCrypto.cpp
    JZElapsedTimer.h
    JZElapsedTimer.cpp
    JZConditionVariable.h
    JZConditionVariable.cpp
    JZTimer.h
    JZTimer.cpp
    JZDebug.h
    JZDebug.cpp
    detail/JZDebugDetail.h
    detail/JZDebugDetail.cpp
    JZUuid.h
    JZUuid.cpp
    JZMeta.h
    JZMeta.cpp
    JZVariant.h
    JZVariant.cpp
    JZSharedBuffer.h
    JZSharedBuffer.cpp
    JZSettings.h
	JZSettings.cpp
	JZResult.h
	JZResult.cpp
	JZOptional.h
	JZOptional.cpp
	JZScopeGuard.h
	JZScopeGuard.cpp
	JZCommandLineParser.h
	JZCommandLineParser.cpp
    JZConsoleInput.h
    JZConsoleInput.cpp
	JZDeadlineTimer.h
	JZDeadlineTimer.cpp
    JZCircleBuffer.cpp
    JZCircleBuffer.h
	JZLockPointer.h
)

if(WIN32)
    list(APPEND JZCORE_SOURCES JZPlatformWin32.cpp)
endif()

# 平台驱动
if(ESP_PLATFORM)
    list(APPEND JZCORE_SOURCES JZPlatformWrapperEsp32.cpp)
endif()

# ---- base64 (libbase64, 直接源码引用, 非独立库) ----
set(BASE64_DIR "${CMAKE_CURRENT_LIST_DIR}/base64")
list(APPEND JZCORE_SOURCES
    ${BASE64_DIR}/lib/lib.c
    ${BASE64_DIR}/lib/codec_choose.c
    ${BASE64_DIR}/lib/tables/tables.c
    ${BASE64_DIR}/lib/arch/generic/codec.c
    ${BASE64_DIR}/lib/arch/ssse3/codec.c
    ${BASE64_DIR}/lib/arch/sse41/codec.c
    ${BASE64_DIR}/lib/arch/sse42/codec.c
    ${BASE64_DIR}/lib/arch/avx/codec.c
    ${BASE64_DIR}/lib/arch/avx2/codec.c
    ${BASE64_DIR}/lib/arch/avx512/codec.c
    ${BASE64_DIR}/lib/arch/neon32/codec.c
    ${BASE64_DIR}/lib/arch/neon64/codec.c)

convert_to_abs_paths(JZCORE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCORE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${BASE64_DIR}/include")
target_include_directories(${JZ_MODULE_TARGET} PRIVATE "${BASE64_DIR}/lib")
target_compile_definitions(${JZ_MODULE_TARGET} PRIVATE BASE64_STATIC_DEFINE _SCL_SECURE_NO_WARNINGS)

# base64 SIMD 逐文件编译选项 (与原库 CMake 一致)
# 仅 x86 平台启用 SIMD 编译选项，ESP32 平台由 generic 实现处理
if(NOT ESP_PLATFORM)
    if(MSVC)
        set_source_files_properties(${BASE64_DIR}/lib/arch/avx/codec.c   PROPERTIES COMPILE_FLAGS "/arch:AVX")
        set_source_files_properties(${BASE64_DIR}/lib/arch/avx2/codec.c  PROPERTIES COMPILE_FLAGS "/arch:AVX2")
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        set_source_files_properties(${BASE64_DIR}/lib/arch/ssse3/codec.c  PROPERTIES COMPILE_FLAGS "-mssse3")
        set_source_files_properties(${BASE64_DIR}/lib/arch/sse41/codec.c  PROPERTIES COMPILE_FLAGS "-msse4.1")
        set_source_files_properties(${BASE64_DIR}/lib/arch/sse42/codec.c  PROPERTIES COMPILE_FLAGS "-msse4.2")
        set_source_files_properties(${BASE64_DIR}/lib/arch/avx/codec.c    PROPERTIES COMPILE_FLAGS "-mavx")
        set_source_files_properties(${BASE64_DIR}/lib/arch/avx2/codec.c   PROPERTIES COMPILE_FLAGS "-mavx2")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
            set_source_files_properties(${BASE64_DIR}/lib/arch/neon32/codec.c PROPERTIES COMPILE_FLAGS "-mfpu=neon")
        endif()
    endif()
endif()