#include <openssl/hmac.h>
#include "JZHmac.h"

namespace JZCrypto
{

JZByteArray hmacSha256(const char *key, int keyLen, const char *data, int dataLen)
{
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    const unsigned char *r = HMAC(EVP_sha256(),
                                  key, keyLen,
                                  (const unsigned char *)data, (size_t)dataLen,
                                  out, &outLen);
    if (r == NULL)
        return JZByteArray();
    return JZByteArray((const char *)out, (int)outLen);
}

JZByteArray hmacSha256(const JZByteArray &key, const JZByteArray &data)
{
    return hmacSha256(key.data(), key.size(), data.data(), data.size());
}

} // namespace JZCrypto
