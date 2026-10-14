#ifndef JZ_MAT_H_
#define JZ_MAT_H_
#include "JZSharedBuffer.h"

// ⚠ JZCv 是**静态单例模块**(自己的 target, 见根 CMakeLists 的 JZ_SINGLE_MODULES), 不是跨 DLL
//   边界的库 ⇒ 自己的类不加 JZFRAMEWORK_API(动态库模式下那是 dllimport, 静态实现会解析不到)。
class JZMat
{
public:
    JZMat();
    ~JZMat();

protected:
    JZSharedBuffer m_buffer;
};

#endif