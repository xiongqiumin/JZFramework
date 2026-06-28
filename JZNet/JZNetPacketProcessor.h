#ifndef MG_PACKET_MANAGER_H_
#define MG_PACKET_MANAGER_H_

#include "JZNetPacket.h"
#include "JZCore/JZDataStream.h"

class JZNetPacketProcessor
{
public:
    JZNetPacketProcessor();
    ~JZNetPacketProcessor();
    
    void setPassword(std::string token);
    void append(const char *buffer, int data);
    void clear();

    bool parse();
    JZNetPackPtr takePacket();
    JZNetPackPtr takePacketByType(int type);
    void clearPacket();

    JZByteArray formatPacket(const JZNetPack *packet);

private:
    JZByteArray m_buffer;
    std::vector<JZNetPackPtr> m_packList;
    uint32_t m_password;
    uint32_t m_sendShift;
    uint32_t m_recvShift;
};


#endif // ! MG_PACKET_MANAGER_H_
