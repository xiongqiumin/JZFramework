#ifndef JZ_SHA256_H_
#define JZ_SHA256_H_
#include <stddef.h>
#include "JZByteArray.h"

// OpenSSL EVP 实现的 SHA-256, 全部位于 JZCrypto 命名空间下(避免与链接的 OpenSSL libcrypto 符号冲突)。
namespace JZCrypto
{
    // 一次性计算 SHA-256 摘要(32 字节)
    JZFRAMEWORK_API JZByteArray sha256(const void *data, size_t len);
    JZFRAMEWORK_API JZByteArray sha256(const JZByteArray &data);

    // 增量式 SHA-256: 适合大文件 / 流式数据
    class JZFRAMEWORK_API JZSha256
    {
    public:
        JZSha256();
        ~JZSha256();

        void reset();
        void update(const void *data, size_t len);
        void update(const JZByteArray &data);
        JZByteArray final();   // 返回 32 字节摘要

    private:
        JZSha256(const JZSha256 &) = delete;
        JZSha256 &operator=(const JZSha256 &) = delete;

        void *m_ctx;   // EVP_MD_CTX *
    };
}

#endif // !JZ_SHA256_H_
