#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* miniz_export.h -- 定义 MINIZ_EXPORT 可见性宏。
 * 官方 miniz 模块化构建需要此文件（仓库未随源码分发，构建时生成）。
 * 默认（未定义 MINIZ_DLL）MINIZ_EXPORT 展开为空，静态链接即可。
 * 若以 DLL 方式使用 miniz，则通过 MINIZ_DLL + MINIZ_BUILD 控制 dllexport/dllimport。
 */
#ifdef MINIZ_DLL
  #ifdef MINIZ_BUILD
    #define MINIZ_EXPORT __declspec(dllexport)
  #else
    #define MINIZ_EXPORT __declspec(dllimport)
  #endif
#else
  #define MINIZ_EXPORT
#endif

#endif /* MINIZ_EXPORT_H */
