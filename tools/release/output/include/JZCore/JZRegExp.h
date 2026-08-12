#ifndef JZ_REGEXP_H_
#define JZ_REGEXP_H_

#include <vector>
#include <string>
#include <memory>
#include "JZString.h"

//JZRegExpMatch
class JZRegExpMatch
{
public:
    JZRegExpMatch();

    JZString captured(const JZString &subject, int index = 0) const;

    bool matched;
    int start;
    int end;
    std::vector<std::pair<int, int>> captures;
};

//JZRegExp
class JZRegExp
{
public:
    enum Flag {
        Global     = 1 << 0,
        IgnoreCase = 1 << 1,
        MultiLine  = 1 << 2,
        DotAll     = 1 << 3,
        Unicode    = 1 << 4,
        Sticky     = 1 << 5,
    };

    JZRegExp();
    JZRegExp(const JZString& pattern, int flags = 0);
    ~JZRegExp();

    bool compile(const JZString& pattern, int flags = 0);
    bool isValid() const;
    int captureCount() const;
    const std::string& errorString() const;

    bool test(const JZString& input) const;
    JZRegExpMatch exec(const JZString& input, int startIndex = 0) const;

private:
    void clear();

    std::string m_error;
    std::shared_ptr<uint8_t> m_byteCode;
};

#endif // JZ_REGEXP_H_