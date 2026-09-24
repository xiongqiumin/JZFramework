#ifndef JZ_Cryptographic_Hash_H_
#define JZ_Cryptographic_Hash_H_

#include <string>
#include "JZByteArray.h"
#include "crypto/JZMd5.h"
#include "crypto/JZXorShift.h"
#include "crypto/JZSha256.h"
#include "crypto/JZHmac.h"
#include "crypto/JZAes.h"
#include "crypto/JZChaCha20.h"
#include "crypto/JZChaCha20Poly1305.h"

// 便捷门面: 各算法实现已拆分到 crypto/ 目录下独立文件。
// 标准 MD5 接口(MD5_CTX / MD5_Init / MD5_Update / MD5_Final 等)在 JZCrypto 命名空间下, 见 crypto/JZMd5.h。
// SHA-256 (EVP) 见 crypto/JZSha256.h; HMAC-SHA256 见 crypto/JZHmac.h;
// AES-256 (CBC/GCM) 见 crypto/JZAes.h; ChaCha20 流密码见 crypto/JZChaCha20.h。
// 通用哈希入口 hash() / hashFile() / hashFileFast() 的实现见 JZCrypto.cpp。
// 旧调用 JZCrypto::xorshift32Stream 保持不变(弱混淆用, 新代码建议用 JZChaCha20)。
namespace JZCrypto
{
    // 支持的哈希算法
    enum class HashAlgorithm
    {
        MD5 = 0,
        SHA256 = 1,
    };

    // 通用哈希入口: 按算法对内存数据求摘要
    JZByteArray hash(HashAlgorithm algo, const char *buffer, int size);
    JZByteArray hash(HashAlgorithm algo, const JZByteArray &buffer);

    // 对文件求完整摘要(读取整个文件), 适合中小文件 / 需要完整校验的场景。
    JZByteArray hashFile(HashAlgorithm algo, const std::string &path);

    // 对大文件快速求"内容指纹": 自动按文件大小在多个偏移处取样(首/中/尾),
    // 只读取少量数据, 不全读文件。注意: 这是快速指纹, 不是完整密码学摘要,
    // 不能用于防篡改完整性校验(未取样区域被改动不会被发现)。
    JZByteArray hashFileFast(HashAlgorithm algo, const std::string &path);

    // ---- 便捷别名(统一门面, 具体实现见 crypto/ 子文件) ----
    // 注: SHA-256 的 sha256() 自由函数已随 crypto/JZSha256.h 直接暴露(无需别名);
    //     增量式哈希请用 JZSha256 类(update/final)。

    // HMAC-SHA256 (key/data 为原始字节)
    inline JZByteArray hmac(const JZByteArray &key, const JZByteArray &data) { return hmacSha256(key, data); }

    // AES-256-CBC (PKCS7 填充): key 32 字节, iv 16 字节
    inline JZByteArray aesCbcEncrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &plain) { return aes256CbcEncrypt(key, iv, plain); }
    inline JZByteArray aesCbcDecrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &cipher) { return aes256CbcDecrypt(key, iv, cipher); }

    // AES-256-GCM (认证加密): key 32 字节, iv 建议 12 字节; 密文尾部附加 16 字节 tag
    inline JZByteArray aesGcmEncrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &plain, const JZByteArray &aad = JZByteArray()) { return aes256GcmEncrypt(key, iv, plain, aad); }
    inline JZByteArray aesGcmDecrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &cipher, const JZByteArray &aad = JZByteArray()) { return aes256GcmDecrypt(key, iv, cipher, aad); }

    // ChaCha20 一次性加/解密(同一操作); key 32 字节, nonce 12 字节
    inline JZByteArray chacha20Crypt(const JZByteArray &key, const JZByteArray &nonce, const JZByteArray &data) { return JZChaCha20::crypt(key, nonce, data); }

    // ChaCha20-Poly1305 AEAD: 加密产出 密文 + 16 字节 tag; 解密校验 tag, 失败返回空
    // key 32 字节, nonce 12 字节(每报文必须唯一)
    inline JZByteArray chacha20Poly1305Encrypt(const JZByteArray &key, const JZByteArray &nonce,
                                               const JZByteArray &aad, const JZByteArray &plaintext)
    { return JZChaCha20Poly1305::encrypt(key, nonce, aad, plaintext); }
    inline JZByteArray chacha20Poly1305Decrypt(const JZByteArray &key, const JZByteArray &nonce,
                                               const JZByteArray &aad, const JZByteArray &ciphertextWithTag)
    { return JZChaCha20Poly1305::decrypt(key, nonce, aad, ciphertextWithTag); }

    // 原地异或(弱混淆, 不推荐用于安全场景)
    inline void xorshift32Stream(char *buffer, int size, uint32_t &rngState)
    {
        JZXorShift::stream(buffer, size, rngState);
    }
}

#endif // !JZ_Cryptographic_Hash_H_
