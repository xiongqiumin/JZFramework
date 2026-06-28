#include <assert.h>
#include <string.h>
#include "JZCrypto.h"
#include "JZFile.h"

namespace
{

// MD5 上下文结构体（存储中间计算状态）
typedef struct {
    uint32_t state[4];        // 4个32位寄存器：A, B, C, D
    uint32_t count[2];        // 64位计数器：记录已处理的比特数（低32位+高32位）
    unsigned char buffer[64]; // 数据缓冲区（最多存储64字节，凑齐512位块）
} MD5_CTX;

// 初始向量（A, B, C, D的初始值）
static const uint32_t MD5_INIT_STATE[4] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
};

// 四轮运算使用的常量K（64个32位整数）
static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};

// 每轮的移位量（64步）
static const int S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

// 左循环移位
static inline uint32_t rotate_left(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

// 四轮运算的非线性函数
static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

// 处理单个512位块（64字节）
static void md5_process_block(MD5_CTX *ctx, const unsigned char block[64]) {
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t M[16]; // 将64字节块转换为16个32位整数（小端序）

                    // 64字节 -> 16个32位整数（小端序）
    for (int i = 0; i < 16; ++i) {
        M[i] = (uint32_t)block[i * 4] |
            ((uint32_t)block[i * 4 + 1] << 8) |
            ((uint32_t)block[i * 4 + 2] << 16) |
            ((uint32_t)block[i * 4 + 3] << 24);
    }

    // 第一轮：F函数
#define FF(a, b, c, d, k, s, i) \
        a = b + rotate_left(a + F(b, c, d) + M[k] + K[i], s)
    FF(a, b, c, d, 0, S[0], 0);  FF(d, a, b, c, 1, S[1], 1);
    FF(c, d, a, b, 2, S[2], 2);  FF(b, c, d, a, 3, S[3], 3);
    FF(a, b, c, d, 4, S[4], 4);  FF(d, a, b, c, 5, S[5], 5);
    FF(c, d, a, b, 6, S[6], 6);  FF(b, c, d, a, 7, S[7], 7);
    FF(a, b, c, d, 8, S[8], 8);  FF(d, a, b, c, 9, S[9], 9);
    FF(c, d, a, b, 10, S[10], 10); FF(b, c, d, a, 11, S[11], 11);
    FF(a, b, c, d, 12, S[12], 12); FF(d, a, b, c, 13, S[13], 13);
    FF(c, d, a, b, 14, S[14], 14); FF(b, c, d, a, 15, S[15], 15);
#undef FF

    // 第二轮：G函数
#define GG(a, b, c, d, k, s, i) \
        a = b + rotate_left(a + G(b, c, d) + M[k] + K[i], s)
    GG(a, b, c, d, 1, S[16], 16); GG(d, a, b, c, 6, S[17], 17);
    GG(c, d, a, b, 11, S[18], 18); GG(b, c, d, a, 0, S[19], 19);
    GG(a, b, c, d, 5, S[20], 20); GG(d, a, b, c, 10, S[21], 21);
    GG(c, d, a, b, 15, S[22], 22); GG(b, c, d, a, 4, S[23], 23);
    GG(a, b, c, d, 9, S[24], 24); GG(d, a, b, c, 14, S[25], 25);
    GG(c, d, a, b, 3, S[26], 26); GG(b, c, d, a, 8, S[27], 27);
    GG(a, b, c, d, 13, S[28], 28); GG(d, a, b, c, 2, S[29], 29);
    GG(c, d, a, b, 7, S[30], 30); GG(b, c, d, a, 12, S[31], 31);
#undef GG

    // 第三轮：H函数
#define HH(a, b, c, d, k, s, i) \
        a = b + rotate_left(a + H(b, c, d) + M[k] + K[i], s)
    HH(a, b, c, d, 5, S[32], 32); HH(d, a, b, c, 8, S[33], 33);
    HH(c, d, a, b, 11, S[34], 34); HH(b, c, d, a, 14, S[35], 35);
    HH(a, b, c, d, 1, S[36], 36); HH(d, a, b, c, 4, S[37], 37);
    HH(c, d, a, b, 7, S[38], 38); HH(b, c, d, a, 10, S[39], 39);
    HH(a, b, c, d, 13, S[40], 40); HH(d, a, b, c, 0, S[41], 41);
    HH(c, d, a, b, 3, S[42], 42); HH(b, c, d, a, 6, S[43], 43);
    HH(a, b, c, d, 9, S[44], 44); HH(d, a, b, c, 12, S[45], 45);
    HH(c, d, a, b, 15, S[46], 46); HH(b, c, d, a, 2, S[47], 47);
#undef HH

    // 第四轮：I函数
#define II(a, b, c, d, k, s, i) \
        a = b + rotate_left(a + I(b, c, d) + M[k] + K[i], s)
    II(a, b, c, d, 0, S[48], 48); II(d, a, b, c, 7, S[49], 49);
    II(c, d, a, b, 14, S[50], 50); II(b, c, d, a, 5, S[51], 51);
    II(a, b, c, d, 12, S[52], 52); II(d, a, b, c, 3, S[53], 53);
    II(c, d, a, b, 10, S[54], 54); II(b, c, d, a, 1, S[55], 55);
    II(a, b, c, d, 8, S[56], 56); II(d, a, b, c, 15, S[57], 57);
    II(c, d, a, b, 6, S[58], 58); II(b, c, d, a, 13, S[59], 59);
    II(a, b, c, d, 4, S[60], 60); II(d, a, b, c, 11, S[61], 61);
    II(c, d, a, b, 2, S[62], 62); II(b, c, d, a, 9, S[63], 63);
#undef II

    // 更新上下文状态
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

// 初始化MD5上下文
void MD5_Init(MD5_CTX *ctx) {
    if (ctx == NULL) return;
    // 初始化状态为标准初始向量
    memcpy(ctx->state, MD5_INIT_STATE, sizeof(ctx->state));
    // 初始化计数器（0比特已处理）
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    // 清空缓冲区
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

// 向MD5上下文添加数据（分块处理）
void MD5_Update(MD5_CTX *ctx, const void *data, size_t len) {
    if (ctx == NULL || data == NULL || len == 0) return;

    const unsigned char *input = (const unsigned char *)data;
    size_t i, buffer_used = (ctx->count[0] >> 3) & 0x3F; // 缓冲区已使用的字节数（0-63）

                                                            // 更新64位计数器（总比特数 = 之前的 + len*8）
    uint32_t lo = ctx->count[0] + ((uint32_t)len << 3);
    if (lo < ctx->count[0]) {
        ctx->count[1]++; // 低32位溢出，高32位加1
    }
    ctx->count[0] = lo;
    ctx->count[1] += (uint32_t)(len >> 29);

    // 如果缓冲区有数据，先填充缓冲区至64字节再处理
    if (buffer_used != 0) {
        size_t fill = 64 - buffer_used;
        if (len < fill) {
            // 数据不足以填满缓冲区，直接存入缓冲区
            memcpy(ctx->buffer + buffer_used, input, len);
            return;
        }
        // 填满缓冲区并处理
        memcpy(ctx->buffer + buffer_used, input, fill);
        md5_process_block(ctx, ctx->buffer);
        input += fill;
        len -= fill;
        buffer_used = 0; // 缓冲区已清空
    }

    // 处理剩余的完整64字节块
    if (len >= 64) {
        size_t blocks = len / 64;
        for (i = 0; i < blocks; i++) {
            md5_process_block(ctx, input + i * 64);
        }
        input += blocks * 64;
        len -= blocks * 64;
    }

    // 剩余数据存入缓冲区
    if (len > 0) {
        memcpy(ctx->buffer, input, len);
    }
}

// 完成MD5计算，获取最终结果（16字节二进制）
void MD5_Final(unsigned char *md, MD5_CTX *ctx) {
    if (md == NULL || ctx == NULL) return;

    unsigned char *p = ctx->buffer;
    size_t buffer_used = (ctx->count[0] >> 3) & 0x3F; // 缓冲区已使用字节数

                                                        // 步骤1：填充0x80
    p[buffer_used++] = 0x80;

    // 步骤2：如果剩余空间不足8字节（存储长度），先处理当前块
    if (buffer_used > 56) {
        // 填充剩余空间为0，处理块
        memset(p + buffer_used, 0, 64 - buffer_used);
        md5_process_block(ctx, p);
        buffer_used = 0; // 重置缓冲区
    }

    // 步骤3：填充0至56字节，最后8字节存储原始长度（小端序）
    memset(p + buffer_used, 0, 56 - buffer_used);
    // 存储64位长度（低32位 + 高32位，小端序）
    p[56] = (unsigned char)(ctx->count[0] & 0xFF);
    p[57] = (unsigned char)((ctx->count[0] >> 8) & 0xFF);
    p[58] = (unsigned char)((ctx->count[0] >> 16) & 0xFF);
    p[59] = (unsigned char)((ctx->count[0] >> 24) & 0xFF);
    p[60] = (unsigned char)(ctx->count[1] & 0xFF);
    p[61] = (unsigned char)((ctx->count[1] >> 8) & 0xFF);
    p[62] = (unsigned char)((ctx->count[1] >> 16) & 0xFF);
    p[63] = (unsigned char)((ctx->count[1] >> 24) & 0xFF);

    // 处理最后一个块
    md5_process_block(ctx, p);

    // 将结果从state（4个32位整数）转换为16字节二进制（小端序）
    for (int i = 0; i < 4; i++) {
        md[i * 4] = (unsigned char)(ctx->state[i] & 0xFF);
        md[i * 4 + 1] = (unsigned char)((ctx->state[i] >> 8) & 0xFF);
        md[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xFF);
        md[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xFF);
    }

    // 清空上下文（可选，增强安全性）
    memset(ctx, 0, sizeof(MD5_CTX));
}
};


JZByteArray JZCrypto::md5(const char *buffer, int size)
{
    MD5_CTX ctx;
    MD5_Init(&ctx);

    MD5_Update(&ctx, buffer, size);

    unsigned char md5[16];
    MD5_Final(md5, &ctx);

    return JZByteArray((char *)md5, 16);
}

JZByteArray JZCrypto::md5(const JZByteArray &buffer)
{
    return md5(buffer.data(), buffer.size());
}

JZByteArray JZCrypto::fileMd5(const string &filepath)
{
    constexpr int len = 8 * 1024;

    JZFile file;
    if(!file.open(filepath.c_str(), "rb"))    
        return JZByteArray();

    MD5_CTX ctx;
    MD5_Init(&ctx);        

    char buffer[len];
    while (true)
    {
        int size = file.read(buffer, len);
        if (size == 0)
            break;

        MD5_Update(&ctx, buffer, size);
    }    
    
    unsigned char md5[16];
    MD5_Final(md5, &ctx);
    file.close();

    return JZByteArray((char*)md5,16);
}

void JZCrypto::xorshift32Stream(char *buffer, int size, uint32_t& rngState)
{
    uint8_t *data = (uint8_t *)buffer;
    size_t len = size;

    for (size_t i = 0; i < len; ++i)
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