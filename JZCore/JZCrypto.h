#ifndef JZ_Cryptographic_Hash_H_
#define JZ_Cryptographic_Hash_H_

#include "JZByteArray.h"

class JZCrypto
{
public:
    static JZByteArray md5(const char *buffer, int size);
    static JZByteArray md5(const JZByteArray &buffer);

    //原地异或
    static void xorshift32Stream(char *buffer, int size, uint32_t &rngState);
};


#endif // !JZ_Cryptographic_Hash_H_
