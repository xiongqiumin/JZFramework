#ifndef JZCO_CONTEXT_H_
#define JZCO_CONTEXT_H_

class JZCoContext;
class JZCoMainContext;
class JZCoCoroutine;

typedef void* CO_CONTEXT_HANDLE;

JZCoMainContext *createMainContext();
void destoryMainContext(JZCoMainContext *ctx);
CO_CONTEXT_HANDLE getMainContextHandle(JZCoMainContext* ctx);

JZCoContext *createContext(JZCoCoroutine *co,int stack_size);
void destoryContext(JZCoContext *ctx);
CO_CONTEXT_HANDLE getCoContextHandle(JZCoContext* ctx);

void switchCoroutine(CO_CONTEXT_HANDLE cur, CO_CONTEXT_HANDLE next);

#endif // ! JZCO_CONTEXT_H_
