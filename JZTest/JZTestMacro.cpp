#include "JZTestMacro.h"

namespace JZTestInternal 
{
    template<>
    std::string toString(const std::nullptr_t&) {
        return "nullptr";
    }
}