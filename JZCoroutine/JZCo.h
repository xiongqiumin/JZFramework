#ifndef JZCO_H_
#define JZCO_H_

#include "JZCoCoroutine.h"

#define jzco_await

void jzco_init();
void jzco_yield();
void jzco_resume(JZCoCoroutine *co);
void jzco_sleep(int ms);
void jzco_waitThread(std::function<void()> func);       //开启一个线程执行任务,并等待结束
void jzco_waitCoroutine(const std::vector<JZCoCoroutinePtr> &co_list); //等待协程执行完毕
void jzco_waitAllCoroutine();

//开启一个协程
JZCoCoroutinePtr jzco_spawn(const std::function<void()>& func, int stack_size = 1024);
void jzco_start(JZCoCoroutinePtr co_ptr);

#endif // ! JZCO_H_
