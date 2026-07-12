set(JZCORE_SOURCES
    JZGlobal.h
    JZGlobal.cpp
	JZString.h
	JZString.cpp
	JZRegExp.h
	JZRegExp.cpp
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

convert_to_abs_paths(JZCORE_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZCORE_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")