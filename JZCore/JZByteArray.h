#ifndef JZ_BYTE_ARRAY_H_
#define JZ_BYTE_ARRAY_H_

#include <vector>
#include <string>
#include "JZGlobal.h"
#include "JZMeta.h"
#include "JZAllocator.h"

class JZDebug;
class JZByteArray
{
public:
    static JZByteArray fromBase64(const JZByteArray &base64);

    JZByteArray();
    JZByteArray(const char *buffer, int len);
    ~JZByteArray();

    // 用户析构会抑制隐式 move, 这里显式默认 move 让 std::move 走指针转移而非深拷贝
    // 同时显式默认 copy: 声明任一 move 操作会删除隐式 copy, 而框架中大量按值拷贝 JZByteArray,
    // 必须保留 copy 语义(深拷贝)
    JZByteArray(const JZByteArray&) = default;
    JZByteArray& operator=(const JZByteArray&) = default;
    JZByteArray(JZByteArray&&) noexcept = default;
    JZByteArray& operator=(JZByteArray&&) noexcept = default;

    JZByteArray toHex() const;
    JZByteArray toBase64() const;

    void swap(JZByteArray& other);

    bool isEmpty() const;
    int size() const;

    void resize(int size);
    void resize(int size, char value);
    void fill(char value);
    void reserve(int size);

    void remove(int offset, int size);

    char *data(); 
    const char *data() const;
    const char *constData() const;

    char *begin();
    char *end();
    const char *begin() const;
    const char *end() const;

    char& operator[](int index) { return m_vec[index]; }
    const char& operator[](int index) const { return m_vec[index]; }

    void push_back(char ch);
    void append(const char *buffer, int len);
    void append(const JZByteArray &buffer);

    JZByteArray left(int size) const;
    JZByteArray mid(int start, int len = -1) const;
    void clear();        

private:
    typedef std::vector<char, JZAllocator<char>> JZByteVector;

    JZByteVector m_vec;
};
JZ_DECLARE_METATYPE_INTERNAL(JZByteArray, JZMetaType::ByteArray);
JZDebug operator<<(JZDebug d, const JZByteArray &bytes);

JZByteArray str_toUtf8(const std::string &str);
std::string str_fromUtf8(const JZByteArray &buffer);

//转换到本地编码, 输入和输出的字符串是utf8
JZByteArray str_toLocal(const std::string &str);
std::string str_fromLocal(const JZByteArray &buffer);

#endif // ! JZ_BYTE_ARRAY_H_
