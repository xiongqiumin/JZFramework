#include <string>
#include <random>
#include <string.h>
#include "JZUuid.h"

JZUuid::JZUuid() {
    // 初始化二进制数据为0
    memset(m_bytes, 0, sizeof(m_bytes));
}

// ========== 随机数生成（核心工具函数） ==========
bool JZUuid::generateRandomBytes(uint8_t* out, size_t len) {
    if (out == nullptr || len == 0) {
        return false;
    }

    // PC/服务器平台：标准C++11随机数（硬件随机数种子）
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist(0, 255);

    for (size_t i = 0; i < len; i++) {
        out[i] = dist(gen)%256;
    }
    return true;
}

// ========== 字符串格式化（内部工具） ==========
void JZUuid::formatString(const uint8_t* bytes, char* out, StringFormat format) {
    if (bytes == nullptr || out == nullptr) {
        *out = '\0';
        return;
    }

    switch (format) {
    case WithBraces:
        // 带大括号：{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
        snprintf(out, 39, // 38字符+结束符
            "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        break;

    case WithoutBraces:
        // 无大括号：xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        snprintf(out, 37, // 36字符+结束符
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        break;

    case Id128:
        // 纯128位ID（无分隔符）：xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
        snprintf(out, 33, // 32字符+结束符
            "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
            bytes[0], bytes[1], bytes[2], bytes[3],
            bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11],
            bytes[12], bytes[13], bytes[14], bytes[15]);
        break;

    default:
        *out = '\0';
        break;
    }
}

// ========== 静态工厂方法：生成UUID v4 ==========
JZUuid JZUuid::createUuid() {
    JZUuid uuid;

    // 1. 生成16字节随机数
    if (!generateRandomBytes(uuid.m_bytes, sizeof(uuid.m_bytes))) {
        return uuid; // 生成失败，返回空UUID
    }

    // 2. 设置UUID v4固定位（RFC4122规范）
    uuid.m_bytes[6] = (uuid.m_bytes[6] & 0x0F) | 0x40; // 版本位：0100
    uuid.m_bytes[8] = (uuid.m_bytes[8] & 0x3F) | 0x80; // 变体位：10

    return uuid;
}

// ========== 转换为字符串 ==========
std::string JZUuid::toString(StringFormat format) {
    char buffer[40] = { 0 }; // 最大38字符（WithBraces）+ 结束符
    formatString(m_bytes, buffer, format);
    return std::string(buffer);
}