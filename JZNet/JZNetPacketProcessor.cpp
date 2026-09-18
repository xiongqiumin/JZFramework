#include <assert.h>
#include <cstring>
#include "JZNetPacketProcessor.h"
#include "JZLog.h"
#include "JZCrypto.h"
#include "JZDebug.h"

//JZNetPacketProcessor
JZNetPacketProcessor::JZNetPacketProcessor()
{
    m_password = 0;
    m_sendShift = 0;
    m_recvShift = 0;
}

JZNetPacketProcessor::~JZNetPacketProcessor()
{
}

void JZNetPacketProcessor::setPassword(std::string token)
{
    JZByteArray md5Result = JZCrypto::hash(JZCrypto::HashAlgorithm::MD5, token.data(), (int)token.size());
    const uint32_t *arr = reinterpret_cast<const uint32_t *>(md5Result.data());
    uint32_t pass = arr[0] ^ arr[1] ^ arr[2] ^ arr[3];
    if (pass == 0)
        pass = 1;

    m_password = pass;
    m_recvShift = m_password;
    m_sendShift = m_password;
}

void JZNetPacketProcessor::append(const char *buffer, int len)
{
    int pre_size = m_buffer.size();
    m_buffer.append(buffer, len);

    if (m_password != 0)
    {
        // 对本次新增的密文原地流式解密，同步推进 m_shift
        char *start = m_buffer.data() + pre_size;
        int cur_len = m_buffer.size() - pre_size;
        JZCrypto::xorshift32Stream(start, cur_len, m_recvShift);
    }
}

JZByteArray JZNetPacketProcessor::formatPacket(const JZNetPack *packet)
{
    JZByteArray buffer;
    JZDataStream s(&buffer, JZ::WriteOnly);
    s.setMaxSize(JZNetPackManager::maxPackSize());   // 写入上限 = 全局包大小上限
    s << (uint8_t)PACK_HEAD;
    s << (int32_t)0;
    s << (int32_t)packet->packetType();
    packet->saveToStream(s);

    // 序列化过程中任一处超出全局上限, 流会立即置错, 这里统一拦截
    if (s.isError())
    {
        LOG_FMT_F("formatPacket: %s (global limit %d)\n",
                  s.errorString().c_str(), JZNetPackManager::maxPackSize());
        return JZByteArray();
    }

    // buffer 底层为 char 缓冲区(1 字节对齐), 直接转 int* 写入是未对齐访问(UB, ARM 会 hardfault)
    // 用 memcpy 写入原生字节序的 int32_t
    int32_t size = (int32_t)buffer.size();
    memcpy(buffer.data() + 1, &size, sizeof(int32_t));

    if (m_password != 0)
        JZCrypto::xorshift32Stream(buffer.data(), buffer.size(), m_sendShift);

    return buffer;
}

void JZNetPacketProcessor::clear()
{
    m_buffer.clear();
}

JZNetPackPtr JZNetPacketProcessor::takePacket()
{
    if (m_packList.size() == 0)
        return JZNetPackPtr();

    auto pkt = m_packList[0];
    m_packList.erase(m_packList.begin());
    return pkt;
}

JZNetPackPtr JZNetPacketProcessor::takePacketByType(int type)
{
    for (int i = 0; i < (int)m_packList.size(); i++)
    {
        if (m_packList[i]->packetType() == type)
        {
            JZNetPackPtr pkt = m_packList[i];
            vec_removeAt(m_packList, i);
            return pkt;
        }
    }
    return JZNetPackPtr();
}

void JZNetPacketProcessor::clearPacket()
{
    m_packList.clear();
}

bool JZNetPacketProcessor::parse()
{
    int min_pack_len = 1 + 4 + 4;
    int buffer_start = 0;
    while (m_buffer.size() - buffer_start >= min_pack_len)
    {
        int start = buffer_start;
        uint8_t head = *((uint8_t*)m_buffer.data());
        if(head != PACK_HEAD)
        {
            LOG_W("pack head error");
            m_buffer.clear();
            return false;
        }

        int32_t packet_size;
        memcpy(&packet_size, m_buffer.data() + start + 1, sizeof(int32_t));   // 未对齐安全读取
        if(packet_size < 0 || packet_size > JZNetPackManager::maxPackSize())
        {
            LOG_FMT_W("pack size error: %d (global limit %d)\n",
                      packet_size, JZNetPackManager::maxPackSize());
            m_buffer.clear();
            return false;
        }

        if (m_buffer.size() < start + packet_size)
            break;                
                
        int32_t type;
        memcpy(&type, m_buffer.data() + start + 5, sizeof(int32_t));   // 未对齐安全读取
        JZNetPackPtr pack = JZNetPackManager::instance()->createPack(type);
        if(!pack)
        {
            LOG_FMT_W("create pack failed type = %d\n", type);            
            m_buffer.clear();
            return false;
        }

        JZDataStream s(&m_buffer, JZ::ReadOnly);
        s.skipRawData(start + 9);
        pack->loadFromStream(s); 
        m_packList.push_back(pack);

        buffer_start = start + packet_size;
    }
    if (buffer_start != 0)
        m_buffer = m_buffer.mid(buffer_start);

    return true;
}