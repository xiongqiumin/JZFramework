set(JZNET_SOURCES
    JZNetObject.cpp
    JZNetObject.h
	JZUrl.cpp
	JZUrl.h
	JZTcpSocket.cpp
	JZTcpSocket.h
	JZTcpServer.cpp
	JZTcpServer.h
	JZUdpSocket.cpp
	JZUdpSocket.h
    JZNetClient.cpp
    JZNetClient.h
    JZNetPacket.cpp
    JZNetPacket.h
    JZNetPacketProcessor.cpp
    JZNetPacketProcessor.h
    JZNetServer.cpp
    JZNetServer.h
    JZNetFileTrans.cpp
    JZNetFileTrans.h
    JZNetworkAccessManager.cpp
    JZNetworkAccessManager.h
    JZNetworkRequest.h
    JZNetworkRequest.cpp
    JZNetworkReply.h
    JZNetworkReply.cpp
	JZWebSocket.cpp
    JZWebSocket.h
	JZWebSocketServer.cpp
	JZWebSocketServer.h
	JZMqttClient.h
    JZMqttClient.cpp
    JZMqttServer.h
    JZMqttServer.cpp
	JZMqttPacket.cpp
	JZMqttPacket.h
	JZTxClient.cpp
	JZTxClient.h
    JZNetworkInterface.h
    JZNetworkInterface.cpp
    mongoose.h    
    mongoose.c
)
convert_to_abs_paths(JZNET_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZNET_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# Windows: 需要 iphlpapi.lib (GetAdaptersAddresses)
if(WIN32)
    target_link_libraries(${JZ_MODULE_TARGET} PRIVATE iphlpapi)
endif()