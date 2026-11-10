#ifndef JZCO_CONTEXT_H_
#define JZCO_CONTEXT_H_

#include "JZGlobal.h"

class JZCoContext;
class JZCoMainContext;
class JZCoCoroutine;

typedef void* CO_CONTEXT_HANDLE;

// 自由函数: 动态库模式下必须逐个带 JZFRAMEWORK_API(dllexport/dllimport),
// 否则使用方(test/co、binary 里的协程宿主)链接期 LNK2019。
JZFRAMEWORK_API JZCoMainContext *createMainContext();
JZFRAMEWORK_API void destoryMainContext(JZCoMainContext *ctx);
JZFRAMEWORK_API CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx);

JZFRAMEWORK_API JZCoContext *createContext(JZCoCoroutine *co,int stack_size);
JZFRAMEWORK_API void destoryContext(JZCoContext *ctx);
JZFRAMEWORK_API CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx);

JZFRAMEWORK_API void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next);

#endif // ! JZCO_CONTEXT_H_
