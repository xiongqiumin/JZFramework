#include <string.h>
#include <openssl/evp.h>
#include "JZSha256.h"

namespace JZCrypto
{

JZByteArray sha256(const void *data, size_t len)
{
    JZSha256 ctx;
    ctx.update(data, len);
    return ctx.final();
}

JZByteArray sha256(const JZByteArray &data)
{
    return sha256(data.data(), (size_t)data.size());
}

JZSha256::JZSha256()
{
    m_ctx = EVP_MD_CTX_new();
    if (m_ctx)
        EVP_DigestInit_ex((EVP_MD_CTX *)m_ctx, EVP_sha256(), NULL);
}

JZSha256::~JZSha256()
{
    if (m_ctx)
        EVP_MD_CTX_free((EVP_MD_CTX *)m_ctx);
}

void JZSha256::reset()
{
    EVP_DigestInit_ex((EVP_MD_CTX *)m_ctx, EVP_sha256(), NULL);
}

void JZSha256::update(const void *data, size_t len)
{
    if (data == NULL || len == 0)
        return;
    EVP_DigestUpdate((EVP_MD_CTX *)m_ctx, data, len);
}

void JZSha256::update(const JZByteArray &data)
{
    update(data.data(), (size_t)data.size());
}

JZByteArray JZSha256::final()
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    EVP_DigestFinal_ex((EVP_MD_CTX *)m_ctx, md, &mdlen);
    return JZByteArray((const char *)md, (int)mdlen);
}

} // namespace JZCrypto
