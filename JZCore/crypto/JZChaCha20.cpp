#include <string.h>
#include <openssl/evp.h>
#include "JZChaCha20.h"

namespace JZCrypto
{

JZChaCha20::JZChaCha20()
{
    m_ctx = NULL;
}

JZChaCha20::~JZChaCha20()
{
    if (m_ctx)
        EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)m_ctx);
}

JZChaCha20::JZChaCha20(JZChaCha20 &&o) noexcept
    : m_ctx(o.m_ctx)
{
    o.m_ctx = nullptr;
}

JZChaCha20 &JZChaCha20::operator=(JZChaCha20 &&o) noexcept
{
    if (this != &o)
    {
        if (m_ctx)
            EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)m_ctx);
        m_ctx = o.m_ctx;
        o.m_ctx = nullptr;
    }
    return *this;
}

bool JZChaCha20::init(const JZByteArray &key, const JZByteArray &nonce)
{
    if (key.size() != 32 || nonce.size() != 12)
        return false;

    if (!m_ctx)
        m_ctx = EVP_CIPHER_CTX_new();
    if (!m_ctx)
        return false;

    if (EVP_EncryptInit_ex((EVP_CIPHER_CTX *)m_ctx, EVP_chacha20(), NULL,
                           (const unsigned char *)key.data(),
                           (const unsigned char *)nonce.data()) != 1)
    {
        EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)m_ctx);
        m_ctx = NULL;
        return false;
    }
    return true;
}

void JZChaCha20::cryptInPlace(char *buffer, int size)
{
    if (!m_ctx || buffer == NULL || size <= 0)
        return;

    // 流密码: 用全 0 明文驱动 EVP, 输出的密文即密钥流, 与明文(全 0)异或得到密钥流本身。
    // 连续调用 EVP_EncryptUpdate 会自然延续 64 字节块计数器, 形成连续密钥流。
    JZByteArray zeros;
    zeros.resize(size, '\0');
    JZByteArray ks;
    ks.resize(size, '\0');

    int outl = 0;
    EVP_EncryptUpdate((EVP_CIPHER_CTX *)m_ctx, (unsigned char *)ks.data(), &outl, (const unsigned char *)zeros.data(), size);

    unsigned char *dst = (unsigned char *)buffer;
    const unsigned char *k = (const unsigned char *)ks.data();
    for (int i = 0; i < size; ++i)
        dst[i] ^= k[i];
}

void JZChaCha20::cryptInPlace(JZByteArray &buffer)
{
    cryptInPlace(buffer.data(), buffer.size());
}

JZByteArray JZChaCha20::crypt(const JZByteArray &key, const JZByteArray &nonce, const JZByteArray &data)
{
    JZChaCha20 c;
    if (!c.init(key, nonce))
        return JZByteArray();

    JZByteArray out = data;
    c.cryptInPlace(out);
    return out;
}

} // namespace JZCrypto
