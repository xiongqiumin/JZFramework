// detail/JZDebugDetail.h
//
// JZDebug 与 JZOptional / JZResult 的流式输出算子 (operator<<) 定义集中处.
//
// 设计 (零耦合 + 任意编译器安全):
//   本文件被 JZDebug.h / JZOptional.h / JZResult.h 同时 #include, 但自身
//   故意不使用 #pragma once / include guard, 以便被多次包含.
//   可见性通过各 header 自带的 include guard 宏判断:
//       JZ_DEBUG_H_     -> JZDebug 已可见
//       JZ_OPTIONAL_H_  -> JZOptional 已可见
//       JZ_RESULT_H_    -> JZResult 已可见
//   只有当 "JZ_DEBUG_H_ 与某类型 guard 同时定义" 时, 对应算子才会被定义;
//   此时两个类型都是完整类型, 不依赖模板的延迟实例化, 对 MSVC / GCC / Clang 均安全.
//   各算子用 *_DEFINED 宏防止同一 TU 内重复定义 (ODR); 模板跨 TU 的重复定义
//   由 C++ 模板 ODR 规则兜底 (只要 token 序列一致).
//
//   因此: JZDebug.h 不 #include JZOptional.h / JZResult.h,
//         JZOptional.h / JZResult.h 也不 #include JZDebug.h. 三者零耦合,
//         也无需额外定义任何宏.

#ifdef JZ_DEBUG_H_

#include <type_traits>

#  ifdef JZ_OPTIONAL_H_
#    ifndef JZ_DEBUG_OPTIONAL_OP_DEFINED
#      define JZ_DEBUG_OPTIONAL_OP_DEFINED

// 主模板：非 void 类型
template<typename T>
JZDebug operator<<(JZDebug d, const JZOptional<T>& opt)
{
    bool spaceFlag = d.isSpace();
    d.setSpace(false);
    if (opt.isNone())
        d << "None";
    else
        d << "Some(" << opt.value() << ")";
    d.setSpace(spaceFlag);
    return d;
}

// 特化：void 类型 (显式特化非模板, 定义见 JZDebugDetail.cpp, 此处仅声明)
// ⚠ 定义在 .cpp 里 ⇒ 动态库模式下必须带 JZFRAMEWORK_API, 否则使用方 LNK2019。
template<>
JZFRAMEWORK_API JZDebug operator<<(JZDebug d, const JZOptional<void>& opt);

#    endif
#  endif

#  ifdef JZ_RESULT_H_
#    ifndef JZ_DEBUG_RESULT_OP_DEFINED
#      define JZ_DEBUG_RESULT_OP_DEFINED

// 主模板：非 void 类型
template<typename T>
JZDebug operator<<(JZDebug d, const JZResult<T>& r)
{
    bool spaceFlag = d.isSpace();
    d.setSpace(false);
    if (r.isOk())
        d << "Ok(" << r.value() << ")";
    else
        d << "Error(" << r.errorCode() << ":" << r.errorMessage() << ")";
    d.setSpace(spaceFlag);
    return d;
}

// 特化：void 类型 (显式特化非模板, 定义见 JZDebugDetail.cpp, 此处仅声明)
template<>
JZFRAMEWORK_API JZDebug operator<<(JZDebug d, const JZResult<void>& r);

#    endif
#  endif

#endif // JZ_DEBUG_H_
