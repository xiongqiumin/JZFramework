#ifndef JZ_CHACHA20_POLY1305_H_
#define JZ_CHACHA20_POLY1305_H_

#include "JZByteArray.h"

// RFC 8439 ChaCha20-Poly1305 AEAD (OpenSSL EVP_chacha20_poly1305 实现)。
// 认证加密: 加密产出 = 密文(与明文等长) + 16 字节 tag; 解密会校验 tag, 失败返回空。
// 注意: nonce 必须每报文唯一(否则同一 key+nonce 下 ChaCha20 密钥流复用, 灾难性),
//       且 key 必须保密、不可跨报文复用同一 Poly1305 key(RFC 8439 内部已按 nonce 派生)。
namespace JZCrypto
{
    class JZChaCha20Poly1305
    {
    public:
        // key 32 字节, nonce 12 字节(RFC 8439), aad 可为空。
        // 返回 密文(与 plaintext 等长) + 16 字节 tag; 参数错误返回空。
        static JZByteArray encrypt(const JZByteArray &key, const JZByteArray &nonce,
                                   const JZByteArray &aad, const JZByteArray &plaintext);

        // 校验 tag(恒定时间), 成功返回 plaintext, 失败(篡改 / 长度不足 / 校验不通过)返回空。
        // ciphertextWithTag = encrypt() 的输出(末 16 字节为 tag)。
        static JZByteArray decrypt(const JZByteArray &key, const JZByteArray &nonce,
                                   const JZByteArray &aad, const JZByteArray &ciphertextWithTag);
    };
}

#endif // !JZ_CHACHA20_POLY1305_H_
