#ifndef JZ_AES_H_
#define JZ_AES_H_

#include "JZByteArray.h"

// OpenSSL EVP 实现的 AES(256 位), 全部位于 JZCrypto 命名空间下。
namespace JZCrypto
{
    // ============ AES-256-CBC (PKCS7 填充) ============
    // key 必须为 32 字节; iv 必须为 16 字节。
    JZByteArray aes256CbcEncrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &plain);
    JZByteArray aes256CbcDecrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &cipher);

    // ============ AES-256-GCM (认证加密) ============
    // key 必须为 32 字节; iv 建议 12 字节。
    // 密文尾部附加 16 字节认证标签(格式: cipher || tag); decrypt 会校验标签, 失败返回空。
    JZByteArray aes256GcmEncrypt(const JZByteArray &key, const JZByteArray &iv,
                                 const JZByteArray &plain, const JZByteArray &aad = JZByteArray());
    JZByteArray aes256GcmDecrypt(const JZByteArray &key, const JZByteArray &iv,
                                 const JZByteArray &cipherWithTag, const JZByteArray &aad = JZByteArray());
}

#endif // !JZ_AES_H_
