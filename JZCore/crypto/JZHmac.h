#ifndef JZ_HMAC_H_
#define JZ_HMAC_H_

#include "JZByteArray.h"

// OpenSSL 实现的 HMAC(默认 SHA-256), 全部位于 JZCrypto 命名空间下。
namespace JZCrypto
{
    // HMAC-SHA256, 返回 32 字节
    JZByteArray hmacSha256(const JZByteArray &key, const JZByteArray &data);
    JZByteArray hmacSha256(const char *key, int keyLen, const char *data, int dataLen);
}

#endif // !JZ_HMAC_H_
