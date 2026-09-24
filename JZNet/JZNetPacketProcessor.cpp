#include <assert.h>
#include <cstring>
#include "JZNetPacketProcessor.h"
#include "JZLog.h"
#include "JZCrypto.h"
#include "JZDebug.h"

//JZNetPacketProcessor
JZNetPacketProcessor::JZNetPacketProcessor()
{
    m_cryptoOn = false;
}

JZNetPacketProcessor::~JZNetPacketProcessor()
{
}

void JZNetPacketProcessor::deriveKeyNonce(const std::string &token, JZByteArray &key,
                                          JZByteArray &nonceAB, JZByteArray &nonceBA)
{
    // key = SHA256(token) (32 字节)
    key = JZCrypto::sha256(token.data(), token.size());
    // 两个方向各自独立的 12 字节 base nonce(不同盐, 杜绝双向密钥流复用)
    //   nonceAB: client->server   nonceBA: server->client
    std::string s = token + "JZNetPack:ChaCha20Poly1305:AB";
    nonceAB = JZCrypto::sha256(s.data(), s.size()).left(12);
    std::string r = token + "JZNetPack:ChaCha20Poly1305:BA";
    nonceBA = JZCrypto::sha256(r.data(), r.size()).left(12);
}

// 由 base nonce + 报文序号合成每报文唯一 nonce(大端写入末 8 字节)
JZByteArray JZNetPacketProcessor::perPacketNonce(const JZByteArray &base, uint64_t seq)
{
    unsigned char n[12];
    memcpy(n, base.data(), 12);
    for (int i = 0; i < 8; ++i)
        n[4 + i] = (unsigned char)((seq >> (8 * (7 - i))) & 0xff);
    return JZByteArray((const char *)n, 12);
}

void JZNetPacketProcessor::setPassword(std::string token, bool isClient)
{
    if (token.empty())
    {
        m_cryptoOn = false;
        return;
    }

    JZByteArray nonceAB, nonceBA;
    deriveKeyNonce(token, m_key, nonceAB, nonceBA);
    // 收发配对: 发送方与接收方对同一方向的 nonce 必须一致。
    //   client->server 方向: 客户端发(AB) / 服务器收(AB)
    //   server->client 方向: 服务器发(BA) / 客户端收(BA)
    // 两个方向 base nonce 不同(AB != BA), 杜绝双向密钥流复用(two-time-pad)。
    if (isClient)
    {
        m_sendNonce = nonceAB;
        m_recvNonce = nonceBA;
    }
    else
    {
        m_sendNonce = nonceBA;
        m_recvNonce = nonceAB;
    }
    m_sendSeq = 0;
    m_recvSeq = 0;
    m_buffer.clear();    // 丢弃握手阶段残留的明文, 避免切换到加密后误把明文当密文解析
    m_cipherBuf.clear();
    m_cryptoOn = true;
}

void JZNetPacketProcessor::append(const char *buffer, int len)
{
    if (m_cryptoOn)
        m_cipherBuf.append(buffer, len);   // 加密模式: 累积原始密文, 由 parse() 统一解密校验
    else
        m_buffer.append(buffer, len);      // 明文模式(握手 Hand/HandRes)
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

    if (m_cryptoOn)
    {
        JZByteArray nonce = perPacketNonce(m_sendNonce, m_sendSeq);
        int32_t len = (int32_t)buffer.size();
        JZByteArray aad((const char *)&len, 4);   // 4 字节长度前缀同时作为 AAD(防长度被篡改)
        JZByteArray ct = JZCrypto::JZChaCha20Poly1305::encrypt(m_key, nonce, aad, buffer);
        m_sendSeq++;
        if (ct.size() == 0)
            return JZByteArray();
        JZByteArray out;
        out.append((const char *)&len, 4);        // 明文长度前缀: 接收侧据此判定分包边界
        out.append(ct.data(), ct.size());
        return out;
    }

    return buffer;
}

void JZNetPacketProcessor::clear()
{
    m_buffer.clear();
    m_cipherBuf.clear();
}

void JZNetPacketProcessor::resetCrypto()
{
    m_cryptoOn = false;
    m_sendSeq = 0;
    m_recvSeq = 0;
    m_cipherBuf.clear();
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
    // ---- 加密模式: 先从 m_cipherBuf 抽取完整密文包并解密(校验 tag)送入 m_buffer ----
    if (m_cryptoOn)
    {
        for (;;)
        {
            if (m_cipherBuf.size() < 4)
                break;
            int32_t len = 0;
            memcpy(&len, m_cipherBuf.data(), 4);   // 明文长度(= 密文长度, 1:1)
            if (len < 0 || len > JZNetPackManager::maxPackSize())
            {
                LOG_FMT_W("crypto pack len error: %d (global limit %d)\n",
                          len, JZNetPackManager::maxPackSize());
                m_cipherBuf.clear();
                return false;
            }
            int total = 4 + len + 16;               // 前缀 + 密文 + 16 字节 tag
            if (m_cipherBuf.size() < total)         // 还没收齐
                break;

            JZByteArray aad((const char *)m_cipherBuf.data(), 4);
            JZByteArray ct(m_cipherBuf.data() + 4, len + 16);
            JZByteArray plain = JZCrypto::JZChaCha20Poly1305::decrypt(
                m_key, perPacketNonce(m_recvNonce, m_recvSeq), aad, ct);
            m_recvSeq++;
            if (plain.size() == 0)
            {
                LOG_W("JZNetPacketProcessor: ChaCha20-Poly1305 校验失败(篡改或非法密文)");
                m_cipherBuf.clear();
                m_buffer.clear();
                return false;
            }
            m_buffer.append(plain.data(), plain.size());
            m_cipherBuf = m_cipherBuf.mid(total);
        }
    }

    // ---- 明文分包解析(与原逻辑一致) ----
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
