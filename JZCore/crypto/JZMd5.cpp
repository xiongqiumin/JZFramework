#include <assert.h>
#include <string.h>
#include "JZMd5.h"

namespace
{

// 初始向量（A, B, C, D 的初始值）
static const uint32_t MD5_INIT_STATE[4] = {
    0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
};

// 四轮运算使用的常量 K（64 个 32 位整数）
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

// 每轮的移位量（64 步）
static const int S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static inline uint32_t rotate_left(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

// 处理单个 512 位块（64 字节）
static void md5_process_block(JZCrypto::MD5_CTX *ctx, const unsigned char block[64]) {
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t M[16]; // 将 64 字节块转换为 16 个 32 位整数（小端序）

    for (int i = 0; i < 16; ++i) {
        M[i] = (uint32_t)block[i * 4] |
            ((uint32_t)block[i * 4 + 1] << 8) |
            ((uint32_t)block[i * 4 + 2] << 16) |
            ((uint32_t)block[i * 4 + 3] << 24);
    }

    // 第一轮：F 函数
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

    // 第二轮：G 函数
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

    // 第三轮：H 函数
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

    // 第四轮：I 函数
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

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

} // namespace

namespace JZCrypto
{

void MD5_Init(MD5_CTX *ctx)
{
    if (ctx == NULL)
        return;
    memcpy(ctx->state, MD5_INIT_STATE, sizeof(ctx->state));
    ctx->count[0] = 0;
    ctx->count[1] = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void MD5_Update(MD5_CTX *ctx, const void *data, size_t len)
{
    if (ctx == NULL || data == NULL || len == 0)
        return;

    const unsigned char *input = (const unsigned char *)data;
    size_t buffer_used = (ctx->count[0] >> 3) & 0x3F;

    uint32_t lo = ctx->count[0] + ((uint32_t)len << 3);
    if (lo < ctx->count[0])
        ctx->count[1]++;
    ctx->count[0] = lo;
    ctx->count[1] += (uint32_t)(len >> 29);

    if (buffer_used != 0)
    {
        size_t fill = 64 - buffer_used;
        if (len < fill)
        {
            memcpy(ctx->buffer + buffer_used, input, len);
            return;
        }
        memcpy(ctx->buffer + buffer_used, input, fill);
        md5_process_block(ctx, ctx->buffer);
        input += fill;
        len -= fill;
        buffer_used = 0;
    }

    if (len >= 64)
    {
        size_t blocks = len / 64;
        for (size_t i = 0; i < blocks; i++)
            md5_process_block(ctx, input + i * 64);
        input += blocks * 64;
        len -= blocks * 64;
    }

    if (len > 0)
        memcpy(ctx->buffer, input, len);
}

void MD5_Final(unsigned char md[16], MD5_CTX *ctx)
{
    if (md == NULL || ctx == NULL)
        return;

    unsigned char *p = ctx->buffer;
    size_t buffer_used = (ctx->count[0] >> 3) & 0x3F;

    p[buffer_used++] = 0x80;

    if (buffer_used > 56)
    {
        memset(p + buffer_used, 0, 64 - buffer_used);
        md5_process_block(ctx, p);
        buffer_used = 0;
    }

    memset(p + buffer_used, 0, 56 - buffer_used);
    p[56] = (unsigned char)(ctx->count[0] & 0xFF);
    p[57] = (unsigned char)((ctx->count[0] >> 8) & 0xFF);
    p[58] = (unsigned char)((ctx->count[0] >> 16) & 0xFF);
    p[59] = (unsigned char)((ctx->count[0] >> 24) & 0xFF);
    p[60] = (unsigned char)(ctx->count[1] & 0xFF);
    p[61] = (unsigned char)((ctx->count[1] >> 8) & 0xFF);
    p[62] = (unsigned char)((ctx->count[1] >> 16) & 0xFF);
    p[63] = (unsigned char)((ctx->count[1] >> 24) & 0xFF);

    md5_process_block(ctx, p);

    for (int i = 0; i < 4; i++)
    {
        md[i * 4] = (unsigned char)(ctx->state[i] & 0xFF);
        md[i * 4 + 1] = (unsigned char)((ctx->state[i] >> 8) & 0xFF);
        md[i * 4 + 2] = (unsigned char)((ctx->state[i] >> 16) & 0xFF);
        md[i * 4 + 3] = (unsigned char)((ctx->state[i] >> 24) & 0xFF);
    }

    memset(ctx, 0, sizeof(MD5_CTX));
}

void MD5(const void *data, size_t len, unsigned char md[16])
{
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, data, len);
    MD5_Final(md, &ctx);
}

} // namespace JZCrypto
