#include "JZCrypto.h"
#include "JZFile.h"

namespace JZCrypto
{
namespace
{
    // 文件快速指纹取样参数(实现细节, 不对外暴露)
    const int kSampleBlock = 64 * 1024;              // 每个取样块 64KB
    const int kSampleCount = 4;                      // 中部均匀取样点数
    const int kFastThreshold = 8 * 1024 * 1024;      // 超过 8MB 才走快速取样

    // 按算法对一段内存求摘要(实现细节, 不对外暴露)
    JZByteArray computeHash(HashAlgorithm algo, const char *data, int size)
    {
        if (algo == HashAlgorithm::SHA256)
            return sha256(data, (size_t)size);

        // 默认 MD5
        unsigned char d[16];
        MD5(data, (size_t)size, d);
        return JZByteArray((const char *)d, 16);
    }
} // namespace

JZByteArray hash(HashAlgorithm algo, const char *buffer, int size)
{
    return computeHash(algo, buffer, size);
}

JZByteArray hash(HashAlgorithm algo, const JZByteArray &buffer)
{
    return computeHash(algo, buffer.data(), buffer.size());
}

JZByteArray hashFile(HashAlgorithm algo, const std::string &path)
{
    JZFile f;
    if (!f.open(path, "rb"))
        return JZByteArray();

    char buf[kSampleBlock];
    int n;

    JZByteArray result;
    if (algo == HashAlgorithm::SHA256)
    {
        JZSha256 ctx;
        while ((n = f.read(buf, kSampleBlock)) > 0)
            ctx.update(buf, (size_t)n);
        result = ctx.final();
    }
    else
    {
        MD5_CTX ctx;
        MD5_Init(&ctx);
        while ((n = f.read(buf, kSampleBlock)) > 0)
            MD5_Update(&ctx, buf, (size_t)n);
        unsigned char d[16];
        MD5_Final(d, &ctx);
        result = JZByteArray((const char *)d, 16);
    }

    f.close();
    return result;
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

    JZByteArray sampled;
    char buf[kSampleBlock];
    int n;

    // 首块
    n = f.read(buf, kSampleBlock);
    if (n > 0)
        sampled.append(buf, n);

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
            sampled.append(buf, n);
    }

    // 尾块
    if (size > kSampleBlock)
    {
        f.seek(size - kSampleBlock);
        n = f.read(buf, kSampleBlock);
        if (n > 0)
            sampled.append(buf, n);
    }

    f.close();
    return computeHash(algo, sampled.data(), sampled.size());
}
} // namespace JZCrypto
