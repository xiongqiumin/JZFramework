#include "JZNetworkInterface.h"
#include "JZGlobal.h"
#include <cstdio>
#include <cstring>

#if JZ_OS == JZ_OS_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#elif JZ_OS == JZ_OS_LINUX
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#else
extern "C" {
#include "lwip/netif.h"
}
#endif

std::vector<JZNetworkInterfaceInfo> JZNetworkInterface::allInterfaces()
{
    std::vector<JZNetworkInterfaceInfo> result;

#if JZ_OS == JZ_OS_WINDOWS
    ULONG bufSize = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                         nullptr, nullptr, &bufSize);
    if (bufSize == 0) return result;

    auto *addrs = (IP_ADAPTER_ADDRESSES *)malloc(bufSize);
    if (!addrs) return result;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, addrs, &bufSize) == NO_ERROR)
    {
        for (auto *a = addrs; a; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto *u = a->FirstUnicastAddress; u; u = u->Next)
            {
                auto *addr4 = (sockaddr_in *)u->Address.lpSockaddr;
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
                if (strcmp(buf, "0.0.0.0") == 0 || strncmp(buf, "127.", 4) == 0) continue;
                int len = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nullptr, 0, nullptr, nullptr);
                std::string name(len ? len - 1 : 0, '\0');
                if (len) WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, &name[0], len, nullptr, nullptr);
                result.push_back({name, buf});
            }
        }
    }
    free(addrs);

#elif JZ_OS == JZ_OS_LINUX
    struct ifaddrs *ifas = nullptr;
    if (getifaddrs(&ifas) != 0) return result;

    for (struct ifaddrs *ifa = ifas; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        auto *addr4 = (sockaddr_in *)ifa->ifa_addr;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
        if (strcmp(buf, "0.0.0.0") == 0 || strncmp(buf, "127.", 4) == 0) continue;
        result.push_back({ifa->ifa_name, buf});
    }
    freeifaddrs(ifas);

#else
    for (struct netif *n = netif_list; n; n = n->next)
    {
        if (!netif_is_up(n)) continue;
        auto *ip4 = netif_ip4_addr(n);
        if (ip4_addr_isany(ip4) || ip4_addr_isloopback(ip4)) continue;
        const char *ip_str = ip4addr_ntoa(ip4);
        char name[16];
        snprintf(name, sizeof(name), "%c%c%d", n->name[0], n->name[1], (int)n->num);
        result.push_back({name, ip_str});
    }
#endif

    return result;
}
