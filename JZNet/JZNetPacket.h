#ifndef MG_PACKET_H_
#define MG_PACKET_H_
#include <memory>
#include <map>
#include <string>
#include <assert.h>
#include "JZDataStream.h"

#define PACK_HEAD 0xAA
#define MG_RECV_BUFFER_SIZE (4 * 1024)
#define MG_MAX_PACK_SIZE    (256 * 1024)

enum {
    NetPack_none,
    NetPack_hand,
    NetPack_handRes,
    NetPack_echo,

    NetPack_filePacketStart,    
    NetPack_fileTransReq,
    NetPack_fileTransRes,    
    NetPack_fileDataReq,
    NetPack_fileDataRes,
    NetPack_fileResult,
    NetPack_filePacketEnd,

    FrpPack_Hand,
    FrpPack_HandRes,
    FrpPack_Trans,

    NetPack_user = 1000,
};

class JZFRAMEWORK_API JZNetPack
{
public:
    JZNetPack();
    virtual ~JZNetPack();

    virtual int packetType() const = 0;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);
};

typedef std::shared_ptr<JZNetPack> JZNetPackPtr;
JZ_DECLARE_METATYPE_INTERNAL(JZNetPackPtr, JZMetaType::NetPackPtr);

// JZNetPackHand
class JZFRAMEWORK_API JZNetPackHand : public JZNetPack
{
public:
    JZNetPackHand();
    virtual ~JZNetPackHand();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string nonce;
};

// JZNetPackHandRes
class JZFRAMEWORK_API JZNetPackHandRes : public JZNetPack
{
public:
    JZNetPackHandRes();
    virtual ~JZNetPackHandRes();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);
};

// JZNetPackEcho
class JZFRAMEWORK_API JZNetPackEcho : public JZNetPack
{
public:
    JZNetPackEcho();
    virtual ~JZNetPackEcho();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::string text;
};

template<class T>
JZNetPack *JZNetPackCreate() { return new T(); }

//JZNetPackManager
typedef JZNetPack *(*CreatePackFunc)();
class JZFRAMEWORK_API JZNetPackManager
{
public:
    static JZNetPackManager *instance();

    // 全局包大小上限: 所有 JZNetPacketProcessor 共享, 运行时可配置
    static int maxPackSize() { return s_maxPackSize; }
    static void setMaxPackSize(int size) { if (size > 0) s_maxPackSize = size; }

    template<class T>
    void registPack(int type)
    {
        assert(type != NetPack_none && m_packFactory.find(type) == m_packFactory.end());
        m_packFactory[type] = JZNetPackCreate<T>;
    }

    JZNetPackPtr createPack(int id);

protected:
    JZNetPackManager();

    static int s_maxPackSize;  // 全局最大包大小限制(字节)

    std::map<int, CreatePackFunc> m_packFactory;
};
void JZNetPackInit();

#endif // ! MG_PACKET_H_
