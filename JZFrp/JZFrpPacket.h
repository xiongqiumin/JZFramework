#ifndef JZFRPPACKET_H_
#define JZFRPPACKET_H_

#include "JZNet/JZNetPacket.h"
#include "JZCore/JZByteArray.h"
#include "JZCore/JZDataStream.h"

/* FRP 流程
本地电脑(JZTcpSocket) <-> 公网服务端(JZFrpServer) <-> 嵌入式设备(JZFrpClient) <-> 嵌入式本地服务(JZTcpServer)
*/
enum 
{
    FrpTrans_None,    
    FrpTrans_Connect,    
    FrpTrans_Disconnect,
    FrpTrans_Error,
    FrpTrans_Write,
};

class JZFrpHand : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    std::vector<int> serverPortList; //JZFrpServer 需要监听哪些端口
};

class JZFrpHandRes : public JZNetPack
{
public:
    JZFrpHandRes();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    int code;
    std::string message;
};

class JZFrpTrans : public JZNetPack
{
public:
    JZFrpTrans();
    ~JZFrpTrans();

    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    int type;
    int serverPort;
    int connectId;  //id 
    JZByteArray data;
};

class JZFrpTransRes : public JZNetPack
{
public:
    virtual int packetType() const;
    virtual void saveToStream(JZDataStream &s) const;
    virtual void loadFromStream(JZDataStream &s);

    int type;
    int serverPort;
    int connectId;  //id
};

// 初始化 FRP 数据包管理器
void JZFrpPacketInit();

#endif // JZFRPPACKET_H_