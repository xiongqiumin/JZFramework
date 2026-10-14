#ifndef JZ_MD5_H_
#define JZ_MD5_H_

#include <stddef.h>
#include "JZByteArray.h"

// 标准 OpenSSL 风格 MD5 接口, 全部位于 JZCrypto 命名空间下(避免与链接的 OpenSSL libcrypto 符号冲突)。
namespace JZCrypto
{
    struct JZFRAMEWORK_API MD5_CTX
    {
        uint32_t state[4];        // 4 个 32 位寄存器: A, B, C, D
        uint32_t count[2];        // 64 位计数器: 已处理的比特数(低 32 + 高 32)
        unsigned char buffer[64]; // 数据缓冲区(凑齐 512 位块)
    };

    void MD5_Init(MD5_CTX *ctx);
    void MD5_Update(MD5_CTX *ctx, const void *data, size_t len);
    void MD5_Final(unsigned char md[16], MD5_CTX *ctx);
    void MD5(const void *data, size_t len, unsigned char md[16]);   // 一次性计算
}

#endif // !JZ_MD5_H_
