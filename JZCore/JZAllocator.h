#ifndef JZ_ALLOCATOR_H_
#define JZ_ALLOCATOR_H_

#include <cstddef>
#include <new>
#include <type_traits>
#include <memory>
#include "JZPlatform.h"

#if defined(JZ_OS_NORMAL)
/*
普通平台 (Windows / Linux): 直接复用标准 std::allocator,
不再绕道 JZPlatform::malloc/free, 行为一致且零开销。
*/
template <class T>
using JZAllocator = std::allocator<T>;

#else
/*
嵌入式平台 (esp32 等): 走 JZPlatform::malloc/free 的 STL 兼容分配器。
在 esp32 上 JZPlatform 实例会优先从 PSRAM 分配, 其它平台退化为标准 malloc/free。
可用于 std::vector 等容器, 例如 JZByteArray 内部缓冲区。
*/
template <class T>
class JZAllocator
{
public:
    typedef T value_type;

    // 无状态分配器: move/拷贝均可无条件转移, 且任意两个实例相等
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap            = std::false_type;
    using is_always_equal                        = std::true_type;

    JZAllocator() noexcept {}
    template <class U>
    JZAllocator(const JZAllocator<U> &) noexcept {}

    T* allocate(std::size_t n)
    {
        if (n == 0)
            return nullptr;
        void *p = JZPlatform::malloc(n * sizeof(T));
        if (!p)
        {
            JZ_ASSERT(0);
        }
        return static_cast<T*>(p);
    }

    void deallocate(T *p, std::size_t /*n*/) noexcept
    {
        JZPlatform::free(p);
    }
};

template <class T, class U>
bool operator==(const JZAllocator<T> &, const JZAllocator<U> &) noexcept
{
    return true;
}

template <class T, class U>
bool operator!=(const JZAllocator<T> &, const JZAllocator<U> &) noexcept
{
    return false;
}

#endif // JZ_OS_NORMAL

#endif // !JZ_ALLOCATOR_H_
