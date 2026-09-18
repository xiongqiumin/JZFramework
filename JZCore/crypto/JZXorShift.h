#ifndef JZ_XORSHIFT_H_
#define JZ_XORSHIFT_H_

// Xorshift32 流密码: 用 32 位 RNG 产生的密钥流对 buffer 原地异或(加密/解密是同一操作)。
// rngState 必须在多次调用之间持续持有, 才能形成连续的密钥流(流式)。
//
// 注意: 该算法强度很弱, 仅用于兼容/混淆。生产环境请使用 JZChaCha20 等强流密码。
class JZXorShift
{
public:
    static inline void stream(char *buffer, int size, uint32_t &rngState)
    {
        uint8_t *data = (uint8_t *)buffer;
        for (int i = 0; i < size; ++i)
        {
            // 取低8位作为当前异或密钥字节
            uint8_t keyByte = static_cast<uint8_t>(rngState & 0xFFU);
            data[i] ^= keyByte;

            // 标准 Xorshift32 状态迭代
            uint32_t x = rngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            rngState = x;
        }
    }
};

#endif // !JZ_XORSHIFT_H_
