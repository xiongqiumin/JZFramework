#pragma once
#include <cstdlib>
#include <string>
#include "JZDebug.h"
#include "JZByteArray.h"
#include "JZCoTcpSocket.h"

// 协程基础库测试共用工具 (风格对齐 test/net/net_test_common.h)

// 读取一行(以 '\n' 结尾)的辅助函数, 应对 TCP 流的分片, 各网络测试共用
inline JZByteArray coReadLine(JZCoTcpSocket *s)
{
    JZByteArray buf;
    while (true)
    {
        auto r = s->readAll();
        if (!r.isOk())
            return JZByteArray();
        buf.append(r.value());
        std::string str = str_fromUtf8(buf);
        if (!str.empty() && str.back() == '\n')
            break;
    }
    return buf;
}

// 协程测试自包含的消息编解码 (不依赖 net_test_common.h 的 netMakeMsg/netParseMsg)
// 格式: "<prefix> <seq>\n"
inline std::string coMakeMsg(const std::string &prefix, uint64_t seq)
{
    return prefix + " " + std::to_string(seq) + "\n";
}

inline bool coParseMsg(const std::string &line, const std::string &prefix, uint64_t &seq)
{
    std::string p = prefix + " ";
    if (line.size() < p.size() || line.compare(0, p.size(), p) != 0)
        return false;
    try
    {
        seq = std::stoull(line.substr(p.size()));
    }
    catch (...)
    {
        return false;
    }
    return true;
}

// 任一断言失败立即 exit(1), 终止整个测试进程
inline void coReportError(const std::string &where, const std::string &detail)
{
    jzDebug() << "[FATAL][" << where << "] " << detail;
    std::exit(1);
}

#define CO_TEST_ASSERT(cond, msg)                                            \
    do {                                                                     \
        if (!(cond))                                                         \
            coReportError(__func__, std::string("断言失败: ") + (msg));      \
    } while (0)
