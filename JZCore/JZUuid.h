#ifndef JZ_UUID_H_
#define JZ_UUID_H_
#include "JZGlobal.h"
#include <string>

class JZFRAMEWORK_API JZUuid
{
public:
    enum StringFormat {
        WithBraces, 
        WithoutBraces, 
        Id128
    };

    static JZUuid createUuid();

    JZUuid();
    std::string toString(StringFormat format = WithoutBraces);

protected:
    static bool generateRandomBytes(uint8_t* out, size_t len);
    static void formatString(const uint8_t* bytes, char* out, StringFormat format);

    uint8_t m_bytes[16];
};

#endif // !JZ_UUID_H_
