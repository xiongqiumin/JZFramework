#ifndef JZCO_H_
#define JZCO_H_

#include "JZGlobal.h"
#include "JZCoCoroutine.h"

#define jzco_await

// 自由函数: 动态库模式下必须逐个带 JZFRAMEWORK_API(dllexport/dllimport),
// 否则使用方(test/co、binary 里的协程宿主)链接期 LNK2019。
JZFRAMEWORK_API void jzco_init();
JZFRAMEWORK_API void jzco_yield();
JZFRAMEWORK_API void jzco_resume(JZCoCoroutine *co);
JZFRAMEWORK_API void jzco_sleep(int ms);

//创建一个等待ms的协程
JZFRAMEWORK_API JZCoCoroutinePtr jzco_after(int ms);
//提交任务到线程池执行,并等待结束
JZFRAMEWORK_API void jzco_submitAndWait(const std::function<void()> &func);  
// 等待协程列表中的任意一个执行完毕（先到先得）
JZFRAMEWORK_API JZCoCoroutinePtr jzco_waitAny(const std::vector<JZCoCoroutinePtr> &co_list, int timeout_ms = -1);  
// 等待协程列表中的所有协程执行完毕
JZFRAMEWORK_API bool jzco_waitAll(const std::vector<JZCoCoroutinePtr> &co_list,int timeout_ms = -1);
// 等待所有协程执行完毕
JZFRAMEWORK_API void jzco_waitAllCoroutines();

//开启一个协程
JZFRAMEWORK_API JZCoCoroutinePtr jzco_spawn(const std::function<void()>& func, int stack_size = 1024);
JZFRAMEWORK_API void jzco_start(JZCoCoroutinePtr co_ptr);

#endif // ! JZCO_H_
