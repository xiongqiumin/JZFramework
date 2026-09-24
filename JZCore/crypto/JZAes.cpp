#include <string.h>
#include <openssl/evp.h>
#include "JZAes.h"

namespace JZCrypto
{
namespace
{

int pkcs7Pad(const unsigned char *in, int inLen, int blockSize,
             unsigned char *out /* >= inLen + blockSize */)
{
    int pad = blockSize - (inLen % blockSize);
    if (pad == 0)
        pad = blockSize;
    memcpy(out, in, inLen);
    memset(out + inLen, pad, pad);
    return inLen + pad;
}

int pkcs7Unpad(const unsigned char *in, int inLen, int blockSize)
{
    if (inLen <= 0 || (inLen % blockSize) != 0)
        return -1;
    int pad = in[inLen - 1];
    if (pad <= 0 || pad > blockSize || pad > inLen)
        return -1;
    return inLen - pad;
}

JZByteArray zeros(int n)
{
    JZByteArray b;
    b.resize(n, '\0');
    return b;
}

} // namespace

// ---------------- AES-256-CBC ----------------
// 手动做 PKCS7 填充, 故关闭 EVP 自带填充。
JZByteArray aes256CbcEncrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &plain)
{
    if (key.size() != 32 || iv.size() != 16)
        return JZByteArray();

    int block = 16;
    int paddedLen = plain.size() + block;
    JZByteArray padded = zeros(paddedLen);
    int outLen = pkcs7Pad((const unsigned char *)plain.data(), plain.size(), block,
                          (unsigned char *)padded.data());

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray result = zeros(outLen);
    int len1 = 0, len2 = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                           (const unsigned char *)key.data(),
                           (const unsigned char *)iv.data()) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_EncryptUpdate(ctx, (unsigned char *)result.data(), &len1,
                          (const unsigned char *)padded.data(), outLen) == 1 &&
        EVP_EncryptFinal_ex(ctx, (unsigned char *)result.data() + len1, &len2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();
    return result;
}

JZByteArray aes256CbcDecrypt(const JZByteArray &key, const JZByteArray &iv, const JZByteArray &cipher)
{
    if (key.size() != 32 || iv.size() != 16 || (cipher.size() % 16) != 0)
        return JZByteArray();

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray out = zeros(cipher.size());
    int len1 = 0, len2 = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL,
                           (const unsigned char *)key.data(),
                           (const unsigned char *)iv.data()) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_DecryptUpdate(ctx, (unsigned char *)out.data(), &len1,
                          (const unsigned char *)cipher.data(), cipher.size()) == 1 &&
        EVP_DecryptFinal_ex(ctx, (unsigned char *)out.data() + len1, &len2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();

    int plainLen = pkcs7Unpad((const unsigned char *)out.data(), len1 + len2, 16);
    if (plainLen < 0)
        return JZByteArray();

    out.resize(plainLen);
    return out;
}

// ---------------- AES-256-GCM ----------------
JZByteArray aes256GcmEncrypt(const JZByteArray &key, const JZByteArray &iv,
                             const JZByteArray &plain, const JZByteArray &aad)
{
    if (key.size() != 32)
        return JZByteArray();

    int ivLen = iv.size() > 0 ? iv.size() : 12;
    JZByteArray realIv = iv.size() > 0 ? iv : zeros(12);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray cipher = zeros(plain.size());
    int len1 = 0, len2 = 0;
    unsigned char tag[16];

    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, ivLen, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL,
                           (const unsigned char *)key.data(),
                           (const unsigned char *)realIv.data()) == 1;

    if (ok && aad.size() > 0)
        ok = EVP_EncryptUpdate(ctx, NULL, &len1,
                               (const unsigned char *)aad.data(), aad.size()) == 1;

    if (ok)
        ok = EVP_EncryptUpdate(ctx, (unsigned char *)cipher.data(), &len1,
                               (const unsigned char *)plain.data(), plain.size()) == 1 &&
             EVP_EncryptFinal_ex(ctx, (unsigned char *)cipher.data() + len1, &len2) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;

    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();

    cipher.append((const char *)tag, 16);
    return cipher;
}

JZByteArray aes256GcmDecrypt(const JZByteArray &key, const JZByteArray &iv,
                             const JZByteArray &cipherWithTag, const JZByteArray &aad)
{
    if (key.size() != 32 || cipherWithTag.size() < 16)
        return JZByteArray();

    int ivLen = iv.size() > 0 ? iv.size() : 12;
    JZByteArray realIv = iv.size() > 0 ? iv : zeros(12);

    int cipherLen = cipherWithTag.size() - 16;
    const unsigned char *tag = (const unsigned char *)cipherWithTag.data() + cipherLen;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return JZByteArray();

    JZByteArray plain = zeros(cipherLen);
    int len1 = 0, len2 = 0;

    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, ivLen, NULL) == 1 &&
        EVP_EncryptInit_ex(ctx, NULL, NULL,
                           (const unsigned char *)key.data(),
                           (const unsigned char *)realIv.data()) == 1;

    if (ok && aad.size() > 0)
        ok = EVP_EncryptUpdate(ctx, NULL, &len1,
                               (const unsigned char *)aad.data(), aad.size()) == 1;

    if (ok)
        ok = EVP_DecryptUpdate(ctx, (unsigned char *)plain.data(), &len1,
                               (const unsigned char *)cipherWithTag.data(), cipherLen) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag) == 1 &&
             EVP_DecryptFinal_ex(ctx, (unsigned char *)plain.data() + len1, &len2) == 1;

    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return JZByteArray();

    plain.resize(len1 + len2);
    return plain;
}

} // namespace JZCrypto
