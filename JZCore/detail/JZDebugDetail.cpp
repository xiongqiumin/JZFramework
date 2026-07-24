// detail/JZDebugDetail.cpp
//
// JZDebug 与 JZOptional / JZResult 的 void 特化 operator<< 定义集中处.
// 这两个是模板的显式特化(非模板), 不能放在头文件中被多 TU 包含,
// 否则会 multiple definition. 故仅在此 .cpp 定义一次, 声明见 JZDebugDetail.h.

#include "JZDebug.h"
#include "JZOptional.h"
#include "JZResult.h"

// JZOptional<void> 特化定义
JZDebug operator<<(JZDebug d, const JZOptional<void>& opt)
{
    bool spaceFlag = d.isSpace();
    d.setSpace(false);
    d << (opt.isNone() ? "None" : "Some");
    d.setSpace(spaceFlag);
    return d;
}

// JZResult<void> 特化定义
JZDebug operator<<(JZDebug d, const JZResult<void>& r)
{
    bool spaceFlag = d.isSpace();
    d.setSpace(false);
    d << (r.isOk() ? "Ok" : "Err");
    d.setSpace(spaceFlag);
    return d;
}
