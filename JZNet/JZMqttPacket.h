#ifndef JZ_NET_MQTT_PACKET_H_
#define JZ_NET_MQTT_PACKET_H_

#include <string>
#include <vector>
#include <memory>
#include "JZByteArray.h"

// MQTT5通用属性
struct JZMqttProp
{
    uint8_t propId{};
    uint32_t intVal{};
    std::string key, val;
};

// ① 连接配置：CONNECT报文专用
struct JZMqttConnectOpt
{
    std::string clientId;
    std::string userName, password;
    uint16_t keepAlive = 60;
    uint8_t protoVer = 4;
    bool cleanSession = true;

    // 遗嘱参数
    std::string willTopic, willMsg;
    uint8_t willQos = 0;
    bool willRetain = false;
    std::vector<JZMqttProp> willProps;
};

// ② 发布配置：PUBLISH专用
struct JZMqttPublishOpt
{
    std::string topic;
    std::string payload;
    uint8_t qos = 0;
    bool retain = false;
    int retransmitId = 0;
    std::vector<JZMqttProp> props;
};

// ③ 订阅配置：SUBSCRIBE/UNSUBSCRIBE专用
struct JZMqttSubOpt
{
    std::string topic;
    uint8_t qos = 0;
    std::vector<JZMqttProp> props;
};

// JZMqttMessage
class JZMqttMessage
{
public:
    std::string topic;
    std::string payload;
    JZByteArray rawPacket;

    uint16_t id{};
    uint8_t cmd{};
    uint8_t qos{};
    uint8_t ackCode{};

    // MQTT5属性
    std::vector<JZMqttProp> props;
};

#endif