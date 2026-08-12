#ifndef JZSCOPEGUARD_H
#define JZSCOPEGUARD_H

#include <functional>
#include "JZGlobal.h"

// 作用域守卫核心类：仅支持 std::function<void()> 类型的清理函数
class JZScopeGuard 
{
public:
    explicit JZScopeGuard(std::function<void()> func);
    ~JZScopeGuard();

    void dismiss();
   
private:
    JZ_DISABLE_COPY(JZScopeGuard)

    std::function<void()> m_func;  // 仅存储 void() 类型的清理函数
    bool m_active;                 // 是否激活清理（默认true）
};

#endif  // JZSCOPEGUARD_H