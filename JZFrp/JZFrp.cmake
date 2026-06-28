set(JZFrp_SOURCES    
    JZFrpPacket.cpp
    JZFrpPacket.h
    JZFrpClient.cpp
    JZFrpClient.h
    JZFrpServer.cpp
    JZFrpServer.h
)
convert_to_abs_paths(JZFrp_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZFrp_SOURCES})
target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")