#ifndef JZCO_H_
#define JZCO_H_

#include "JZCoCoroutine.h"

#define jzco_await

void jzco_init();
void jzco_yield();
void jzco_resume(JZCoCoroutine *co);
void jzco_sleep(int ms);
// 等待协程列表中的任意一个执行完毕（先到先得）
void jzco_waitCoroutineAny(const std::vector<JZCoCoroutinePtr> &co_list);  
// 等待协程列表中的所有协程执行完毕
void jzco_waitCoroutineAll(const std::vector<JZCoCoroutinePtr> &co_list);
void jzco_waitAllCoroutine();
void jzco_submitAndWait(std::function<void()> func);   //提交任务到线程池执行,并等待结束

//开启一个协程
JZCoCoroutinePtr jzco_spawn(const std::function<void()>& func, int stack_size = 1024);
void jzco_start(JZCoCoroutinePtr co_ptr);

#endif // ! JZCO_H_
