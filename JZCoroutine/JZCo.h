#ifndef JZCO_H_
#define JZCO_H_

#include "JZCoCoroutine.h"

#define jzco_await

void jzco_init();
void jzco_yield();
void jzco_resume(JZCoCoroutine *co);
void jzco_sleep(int ms);

//创建一个等待ms的协程
JZCoCoroutinePtr jzco_after(int ms);
//提交任务到线程池执行,并等待结束
void jzco_submitAndWait(const std::function<void()> &func);  
// 等待协程列表中的任意一个执行完毕（先到先得）
JZCoCoroutinePtr jzco_waitAny(const std::vector<JZCoCoroutinePtr> &co_list, int timeout_ms = -1);  
// 等待协程列表中的所有协程执行完毕
bool jzco_waitAll(const std::vector<JZCoCoroutinePtr> &co_list,int timeout_ms = -1);
// 等待所有协程执行完毕
void jzco_waitAllCoroutines();

//开启一个协程
JZCoCoroutinePtr jzco_spawn(const std::function<void()>& func, int stack_size = 1024);
void jzco_start(JZCoCoroutinePtr co_ptr);

#endif // ! JZCO_H_
