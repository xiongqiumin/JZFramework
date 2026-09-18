#include "JZCrypto.h"
#include "JZFile.h"

namespace JZCrypto
{
namespace
{
    const int kSampleBlock = 64 * 1024;              // 每个取样块 64KB
    const int kSampleCount = 4;                      // 中部均匀取样点数
    const int kFastThreshold = 8 * 1024 * 1024;      // 超过 8MB 才走快速取样

    struct HashState
    {
        HashAlgorithm algo;
        MD5_CTX md5;
    };

    void hashInit(HashState &st)
    {
        switch (st.algo)
        {
        case HashAlgorithm::MD5: MD5_Init(&st.md5); break;
        default: break;
        }
    }

    void hashUpdate(HashState &st, const void *data, size_t len)
    {
        switch (st.algo)
        {
        case HashAlgorithm::MD5: MD5_Update(&st.md5, data, len); break;
        default: break;
        }
    }

    JZByteArray hashFinal(HashState &st)
    {
        switch (st.algo)
        {
        case HashAlgorithm::MD5:
        {
            unsigned char d[16];
            MD5_Final(d, &st.md5);
            return JZByteArray((const char *)d, 16);
        }
        default: return JZByteArray();
        }
    }
} // namespace

JZByteArray hash(HashAlgorithm algo, const char *buffer, int size)
{
    HashState st{algo};
    hashInit(st);
    if (buffer && size > 0)
        hashUpdate(st, buffer, (size_t)size);
    return hashFinal(st);
}

JZByteArray hash(HashAlgorithm algo, const JZByteArray &buffer)
{
    return hash(algo, buffer.data(), buffer.size());
}

JZByteArray hashFile(HashAlgorithm algo, const std::string &path)
{
    JZFile f;
    if (!f.open(path, "rb"))
        return JZByteArray();

    HashState st{algo};
    hashInit(st);

    char buf[kSampleBlock];
    int n;
    while ((n = f.read(buf, kSampleBlock)) > 0)
        hashUpdate(st, buf, (size_t)n);

    f.close();
    return hashFinal(st);
}

JZByteArray hashFileFast(HashAlgorithm algo, const std::string &path)
{
    JZFile f;
    if (!f.open(path, "rb"))
        return JZByteArray();

    int size = f.size();

    // 小文件 / 取不到大小: 直接走完整哈希(与 hashFile 等价)
    if (size <= kFastThreshold || size <= 0)
    {
        f.close();
        return hashFile(algo, path);
    }

    HashState st{algo};
    hashInit(st);

    char buf[kSampleBlock];
    int n;

    // 首块
    n = f.read(buf, kSampleBlock);
    if (n > 0)
        hashUpdate(st, buf, (size_t)n);

    // 中部均匀取样点: 偏移 = size * i / (count+1)
    for (int i = 1; i <= kSampleCount; ++i)
    {
        int off = size * i / (kSampleCount + 1);
        if (off + kSampleBlock > size)
            off = size - kSampleBlock;
        if (off < kSampleBlock) // 已被首块覆盖
            off = kSampleBlock;
        f.seek(off);
        n = f.read(buf, kSampleBlock);
        if (n > 0)
            hashUpdate(st, buf, (size_t)n);
    }

    // 尾块
    if (size > kSampleBlock)
    {
        f.seek(size - kSampleBlock);
        n = f.read(buf, kSampleBlock);
        if (n > 0)
            hashUpdate(st, buf, (size_t)n);
    }

    f.close();
    return hashFinal(st);
}
} // namespace JZCrypto
