#ifndef JZ_CHACHA20_H_
#define JZ_CHACHA20_H_

#include "JZByteArray.h"

// OpenSSL EVP 实现的 ChaCha20 流密码, 全部位于 JZCrypto 命名空间下。
// 用法与 JZXorShift 一致: 加密 / 解密都是"用密钥流原地异或", 连续调用延续同一段密钥流。
namespace JZCrypto
{
    class JZChaCha20
    {
    public:
        JZChaCha20();
        ~JZChaCha20();

        // 用 key(32 字节) + nonce(12 字节) 初始化, 块计数器从 0 开始。
        // 返回是否初始化成功(参数长度错误返回 false)。
        bool init(const JZByteArray &key, const JZByteArray &nonce);

        // 是否已初始化
        bool isValid() const { return m_ctx != NULL; }

        // 对 buffer 原地异或密钥流(加密 / 解密同一操作)。连续调用会延续密钥流。
        void cryptInPlace(char *buffer, int size);
        void cryptInPlace(JZByteArray &buffer);

        // 一次性: 用 key/nonce 从密钥流开头对 data 异或(内部独立状态)。
        static JZByteArray crypt(const JZByteArray &key, const JZByteArray &nonce, const JZByteArray &data);

    private:
        JZChaCha20(const JZChaCha20 &) = delete;
        JZChaCha20 &operator=(const JZChaCha20 &) = delete;

        // 允许移动(转移 EVP_CTX 所有权), 方便 JZNetPacketProcessor 作为成员被移动
        JZChaCha20(JZChaCha20 &&o) noexcept;
        JZChaCha20 &operator=(JZChaCha20 &&o) noexcept;

        void *m_ctx;   // EVP_CIPHER_CTX *
    };
}

#endif // !JZ_CHACHA20_H_
