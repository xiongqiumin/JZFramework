#ifndef JZ_NETWORK_INTERFACE_H_
#define JZ_NETWORK_INTERFACE_H_

#include <string>
#include <vector>

struct JZNetworkInterfaceInfo
{
    std::string name;  // 接口名: "eth0", "wlan0", "st1"
    std::string ip;    // IPv4 地址: "192.168.1.100"
};

class JZNetworkInterface
{
public:
    // 获取所有活动网卡信息
    static std::vector<JZNetworkInterfaceInfo> allInterfaces();
};

#endif // JZ_NETWORK_INTERFACE_H_
