#ifndef JZ_STRING_H_
#define JZ_STRING_H_
#include <string>
#include <vector>
#include "JZGlobal.h"

class JZDebug;
class JZString;
class JZRegExp;
class JZFRAMEWORK_API JZChar
{
public:
    JZChar();
    JZChar(char c);
    explicit JZChar(char32_t str);
    
    char toChar() const;
    char32_t toUnicode() const;

    bool isUpper() const;
    bool isLower() const;

    JZChar toUpper() const;
    JZChar toLower() const;

    bool isAscii() const;
    bool isDigit() const;
    bool isSpace() const;
    
    bool operator==(const JZChar& other) const;
    bool operator!=(const JZChar& other) const;
    bool operator<(const JZChar& other) const;
    bool operator<=(const JZChar& other) const;
    bool operator>(const JZChar& other) const;
    bool operator>=(const JZChar& other) const;

protected:
    friend JZString;
    friend JZRegExp;

    char32_t data;
};
JZDebug operator<<(JZDebug d, const JZChar& c);

class JZFRAMEWORK_API JZString
{
public:
    static JZString fromStdString(const std::string& stdStr);
    static JZString fromU32String(const std::u32string& stdStr);
    static JZString number(int n);
    static JZString number(double n, char format = 'g', int precision = 6);

    JZString();
    JZString(const char* str);
    JZString(JZChar c);

    bool operator==(const JZString& other) const;
    bool operator!=(const JZString& other) const;
    bool operator<(const JZString& other) const;
    bool operator<=(const JZString& other) const;
    bool operator>(const JZString& other) const;
    bool operator>=(const JZString& other) const;

    JZString& operator+=(const JZString& other);
    JZString& operator+=(const char* str);

    JZString operator+(const JZString& other) const;
    JZString operator+(const char* str) const;

    std::string toStdString() const;
    std::u32string toU32String() const;
    int toInt(bool* ok = nullptr) const;
    double toDouble(bool* ok = nullptr) const;

    void resize(int size, const JZChar& c = JZChar());
    int size() const;
    int length() const;
    int count() const;
    bool isEmpty() const;
    void clear();

    JZChar& operator[](int index);
    const JZChar& operator[](int index) const;
    const JZChar& at(int index) const;

    JZChar* data();
    const JZChar* data() const;

    std::vector<JZChar>::iterator begin();
    std::vector<JZChar>::iterator end();
    std::vector<JZChar>::const_iterator begin() const;
    std::vector<JZChar>::const_iterator end() const;

    void insert(int position, const JZString& str);
    void remove(int position, int length);
    void replace(int position, int length, const JZString& after);
    void replace(const JZString& before, const JZString& after);

    int indexOf(const JZString& str, int from = 0) const;
    int lastIndexOf(const JZString& str, int from = -1) const;
    bool contains(const JZString& str) const;
    bool startsWith(const JZString& str) const;
    bool endsWith(const JZString& str) const;

    int indexOf(JZChar c, int from = 0) const;
    int lastIndexOf(JZChar c, int from = -1) const;

    const JZChar& front() const;
    const JZChar& back() const;

    void push_back(const JZChar& other);
    void pop_back();

    void append(const JZString& other);
    void chop(int n = 1);

    JZString mid(int position, int length = -1) const;
    JZString left(int length) const;
    JZString right(int length) const;

    JZString toUpper() const;
    JZString toLower() const;
    JZString trimmed() const;
    JZString simplified() const;

    std::vector<JZString> split(const JZString& sep, JZ::SplitBehavior behavior = JZ::KeepEmptyParts) const;

    JZString& fill(const JZChar& c);

private:
    std::vector<JZChar> m_chars;
};
JZFRAMEWORK_API JZString operator+(const char* str1, const JZString& str2);
JZDebug operator<<(JZDebug d, const JZString& str);

JZString jzstr_join(const std::vector<JZString>& list, const JZString& sep);

//std::hash and std::equal_to specialization for JZString
namespace std {
    template<>
    struct hash<JZString> {
        size_t operator()(const JZString& s) const noexcept {
            const char32_t* data = reinterpret_cast<const char32_t*>(s.data());
            size_t len = static_cast<size_t>(s.size());

            // FNV-1a 64位常量（所有平台通用）
            uint64_t hash = 14695981039346656037ULL;
            const uint64_t prime = 1099511628211ULL;

            // 逐char32_t码点哈希，和==语义完全对齐
            for (size_t i = 0; i < len; ++i)
            {
                hash ^= static_cast<size_t>(data[i]);
                hash *= prime;
            }
            return static_cast<size_t>(hash);
        }
    };
}

#endif // !JZ_STRING_H_