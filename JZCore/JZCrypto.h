#ifndef JZ_Cryptographic_Hash_H_
#define JZ_Cryptographic_Hash_H_

#include <string>
#include "JZByteArray.h"
#include "crypto/JZMd5.h"
#include "crypto/JZXorShift.h"

// 便捷门面: 各算法实现已拆分到 crypto/ 目录下独立文件。
// 标准 MD5 接口(MD5_CTX / MD5_Init / MD5_Update / MD5_Final 等)在 JZCrypto 命名空间下, 见 crypto/JZMd5.h。
// 通用哈希入口 hash() / hashFile() / hashFileFast() 的实现见 crypto/JZHash.cpp。
// 旧调用 JZCrypto::xorshift32Stream 保持不变。
namespace JZCrypto
{
    // 支持的哈希算法(随框架扩展, SHA256 等后续实现)
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

    // 原地异或
    inline void xorshift32Stream(char *buffer, int size, uint32_t &rngState)
    {
        JZXorShift::stream(buffer, size, rngState);
    }
}

#endif // !JZ_Cryptographic_Hash_H_
