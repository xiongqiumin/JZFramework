#ifndef JZ_UTF8_ITERATOR_H_
#define JZ_UTF8_ITERATOR_H_
#include "JZGlobal.h"

#include <string>
#include <cstdint>
#include <cstddef>

// JZUtf8Iterator
// 轻量、零拷贝的 UTF-8 码点前向迭代器。
// 用于在 std::string (UTF-8) 上按"码点(unicode scalar value)"遍历，
// 同时保留每个码点对应的【字节起始下标】，便于与按字节寻址的引擎
// (如 quickjs 的 libregexp) 互操作。
//
// 用法:
//   JZUtf8Iterator it(u8str);
//   while (it.isValid())
//   {
//       char32_t cp = it.codepoint();   // 当前码点
//       int bytePos = it.byteIndex();   // 该码点在原串中的字节起点
//       ++it;
//   }
class JZFRAMEWORK_API JZUtf8Iterator
{
public:
    explicit JZUtf8Iterator(const std::string& str);

    // 是否还有未消费的码点
    bool isValid() const;

    explicit operator bool() const;

    // 当前码点的 unicode 标量值
    char32_t codepoint() const;

    // 当前码点起始字节下标
    int byteIndex() const;

    // 当前码点占用的字节数 (1~4)
    int byteLength() const;

    // 已遍历的码点数
    int codepointCount() const;

    // 前进到下一个码点
    JZUtf8Iterator& operator++();

    // 跳到指定字节下标 (必须是一个码点的起点)
    void seek(int byteOffset);

    int size() const;

    // ---- 静态工具 ----

    // 解码从 pos 开始的 1 个 UTF-8 码点。
    // 成功返回 true，cp/len 有效；失败(截断或非法)返回 false，cp=U+FFFD, len=1。
    static bool decode(const char* data, size_t size, size_t pos, char32_t& cp, int& len);

    // 将一个码点编码追加到 out (UTF-8)
    static void encode(char32_t cp, std::string& out);

    // 计算 UTF-8 字符串的码点总数
    static int countCodepoints(const std::string& str);

private:
    const char* m_data;
    size_t m_size;
    size_t m_pos;
    int m_count = 0;
};

#endif // JZ_UTF8_ITERATOR_H_
