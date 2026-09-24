#ifndef MG_PACKET_MANAGER_H_
#define MG_PACKET_MANAGER_H_

#include "JZNetPacket.h"
#include "JZCore/JZDataStream.h"
#include "JZCore/JZCrypto.h"

class JZNetPacketProcessor
{
public:
    JZNetPacketProcessor();
    ~JZNetPacketProcessor();
    
    void setPassword(std::string token, bool isClient);
    void append(const char *buffer, int data);
    void clear();

    // 复位加密状态(关闭握手加密), 通常在每次(重)建连、发送明文 Hand 之前调用
    void resetCrypto();

    bool parse();
    JZNetPackPtr takePacket();
    JZNetPackPtr takePacketByType(int type);
    void clearPacket();

    JZByteArray formatPacket(const JZNetPack *packet);

private:
    // 由密码串派生: 32 字节 key, 以及两个方向各自独立的 12 字节 base nonce
    //   nonceAB: client->server 方向  nonceBA: server->client 方向
    // (双方用相同 token 得到相同结果; 两个方向 nonce 不同, 避免双向复用同一密钥流)
    static void deriveKeyNonce(const std::string &token, JZByteArray &key,
                               JZByteArray &nonceAB, JZByteArray &nonceBA);

    // 由 base nonce + 报文序号合成每报文唯一 nonce(大端写入末 8 字节)
    static JZByteArray perPacketNonce(const JZByteArray &base, uint64_t seq);

private:
    JZByteArray m_buffer;        // 解密后的明文累积缓冲(parse 在此解析)
    JZByteArray m_cipherBuf;     // 加密模式下: 接收侧累积的原始密文(长度前缀+密文+tag)
    std::vector<JZNetPackPtr> m_packList;
    bool m_cryptoOn;             // 是否启用握手加密(ChaCha20-Poly1305)
    JZByteArray m_key;           // 32 字节
    JZByteArray m_sendNonce;     // 12 字节(发送方向 base nonce)
    JZByteArray m_recvNonce;     // 12 字节(接收方向 base nonce)
    uint64_t m_sendSeq = 0;      // 发送报文序号(每包 +1)
    uint64_t m_recvSeq = 0;      // 接收报文序号(每包 +1)
};


#endif // ! MG_PACKET_MANAGER_H_
