#include <string.h>
#include <openssl/evp.h>
#include "JZChaCha20Poly1305.h"

namespace JZCrypto
{
namespace
{
    const int kTagSize = 16;

    JZByteArray zeros(int n)
    {
        JZByteArray b;
        b.resize(n, '\0');
        return b;
    }
} // namespace

JZByteArray JZChaCha20Poly1305::encrypt(const JZByteArray &key, const JZByteArray &nonce,
                                        const JZByteArray &aad, const JZByteArray &plaintext)
{
    if (key.size() != 32 || nonce.size() != 12)
        return JZByteArray();

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray cipher = zeros(plaintext.size());
    int len1 = 0, len2 = 0;
    unsigned char tag[kTagSize];

    bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1 &&
              EVP_EncryptInit_ex(ctx, NULL, NULL,
                                 (const unsigned char *)key.data(),
                                 (const unsigned char *)nonce.data()) == 1;

    if (ok && aad.size() > 0)
        ok = EVP_EncryptUpdate(ctx, NULL, &len1,
                               (const unsigned char *)aad.data(), aad.size()) == 1;

    if (ok)
        ok = EVP_EncryptUpdate(ctx, (unsigned char *)cipher.data(), &len1,
                               (const unsigned char *)plaintext.data(), plaintext.size()) == 1 &&
             EVP_EncryptFinal_ex(ctx, (unsigned char *)cipher.data() + len1, &len2) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, kTagSize, tag) == 1;

    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();

    cipher.append((const char *)tag, kTagSize);
    return cipher;
}

JZByteArray JZChaCha20Poly1305::decrypt(const JZByteArray &key, const JZByteArray &nonce,
                                        const JZByteArray &aad, const JZByteArray &ciphertextWithTag)
{
    if (key.size() != 32 || nonce.size() != 12 || ciphertextWithTag.size() < kTagSize)
        return JZByteArray();

    int cipherLen = ciphertextWithTag.size() - kTagSize;
    const unsigned char *tag = (const unsigned char *)ciphertextWithTag.data() + cipherLen;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray plain = zeros(cipherLen);
    int len1 = 0, len2 = 0;

    bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) == 1 &&
              EVP_DecryptInit_ex(ctx, NULL, NULL,
                                 (const unsigned char *)key.data(),
                                 (const unsigned char *)nonce.data()) == 1;

    if (ok && aad.size() > 0)
        ok = EVP_DecryptUpdate(ctx, NULL, &len1,
                               (const unsigned char *)aad.data(), aad.size()) == 1;

    if (ok)
        ok = EVP_DecryptUpdate(ctx, (unsigned char *)plain.data(), &len1,
                               (const unsigned char *)ciphertextWithTag.data(), cipherLen) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, kTagSize, (void *)tag) == 1 &&
             EVP_DecryptFinal_ex(ctx, (unsigned char *)plain.data() + len1, &len2) == 1;

    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();

    plain.resize(len1 + len2);
    return plain;
}

} // namespace JZCrypto
