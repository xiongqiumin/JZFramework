#ifndef JZ_GAME_GLOBAL_H_
#define JZ_GAME_GLOBAL_H_
#pragma once

// 头部同时会用到 JZFRAMEWORK_API(框架类型)与 JZGAME_API(本模块类型), 一并引入。
#include "JZGlobal.h"

// ============================================================================
// JZGAME_API — JZGame 模块的导出宏
//
// JZGame 自 2026-11-27 起**单独编成 JZGame.dll**(不再编进 JZFramework)：
//   * 独立 target ⇒ 只有真正做 2D 游戏/控件的程序才拖这个 DLL；
//   * 导出面只有本模块的类，不和核心库混在一张导出表里。
//
// 与 JZFRAMEWORK_API 的分工:
//   * JZGame 自己的类 (JZGameView / JZGameView2D / JZCamera2D / JZSprite /
//     JZScene / JZBody) 用 JZGAME_API —— 它们活在 JZGame.dll 里；
//   * 用到的框架类型 (JZWidget / JZPainter / JZTimer ...) 仍是 JZFRAMEWORK_API ——
//     它们活在 JZFramework.dll 里, 由 JZGame 去 import。
//
// 开关:
//   * 建库侧: CMake 对 SHARED target 自动定义 JZGame_EXPORTS ⇒ dllexport；
//   * 使用方: target_link_libraries(... JZGame) 会带出 JZGAME_DYNAMIC ⇒ dllimport。
//   两者都没有(静态/源码内联编译)时宏为空, 行为与旧版一致。
// ============================================================================
#if defined(_WIN32)
    #if defined(JZGAME_DYNAMIC)
        #ifdef JZGame_EXPORTS
            #define JZGAME_API __declspec(dllexport)
        #else
            #define JZGAME_API __declspec(dllimport)
        #endif
    #else
        #define JZGAME_API
    #endif
#else
    #if defined(JZGAME_DYNAMIC) && defined(__GNUC__)
        #define JZGAME_API __attribute__((visibility("default")))
    #else
        #define JZGAME_API
    #endif
#endif

#endif // !JZ_GAME_GLOBAL_H_
