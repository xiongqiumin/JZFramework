# ============================================================================
# JZGame: 2D 游戏层 —— 基于 JZWidget 的控件/绘制/键盘/定时器 + 2D 滚屏相机
#
# 自 2026-11-27 起**单独编成 JZGame.dll**, 以"共享模块"形式加入构建:
#     jzframework_use_modules(... SHARED_MODULES JZGame)
# 与普通模块(编进 JZFramework)的区别:
#   * 导出面只有本模块的类(JZGAME_API, 见 JZGameGlobal.h), 核心库的导出表不受影响;
#   * 只有真做 2D 游戏/游戏控件的程序才拖这个 DLL (核心库的消费方无感);
#   * 依赖方向: JZGame -> JZFramework(单向), 核心库**不反向依赖** JZGame。
#
# ⚠ 本模块只在 Windows 下构建 (JZWidget 本身只有 Windows 实现)。
# ⚠ target 由**本文件**创建(而不是 jzframework_use_modules 统一创建): 只有这里才知道
#   "依赖齐了没/能不能建", 提前 return 就不会留下一个空 DLL 目标。
# ============================================================================
if(NOT WIN32)
    message(STATUS "[JZGame] 跳过: 仅 Windows 构建 (JZWidget 只有 Windows 实现)")
    return()
endif()

if(NOT HAS_JZWidget)
    message(STATUS "[JZGame] 跳过: 需要 JZWidget")
    return()
endif()

set(JZGame_SOURCES
    JZGameGlobal.h
    # 控件基类: 两个 timer + 键盘/fps
    JZGameView.h
    JZGameView.cpp
    # 滚屏: 相机 + 带相机的控件基类
    JZCamera2D.h
    JZCamera2D.cpp
    JZGameView2D.h
    JZGameView2D.cpp
    # 2D 三件套: 碰撞体(AABB) / 精灵 / 场景(绘制队列 + 重叠查询 + 剔除绘制)
    JZBody.h
    JZSprite.h
    JZSprite.cpp
    JZScene.h
    JZScene.cpp
)
convert_to_abs_paths(JZGame_SOURCES)

add_library(${JZ_MODULE_TARGET} SHARED ${JZGame_SOURCES})
add_library(JZFramework::JZGame ALIAS ${JZ_MODULE_TARGET})

target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")

# PUBLIC: 公共头里直接出现 JZWidget/JZPainter/JZTimer ⇒ 消费方也要拿到框架的 include
# 与编译宏(JZFRAMEWORK_DYNAMIC/JSON_DLL/HAS_*)。
target_link_libraries(${JZ_MODULE_TARGET} PUBLIC JZFramework)

# 使用方看到 JZGAME_DYNAMIC ⇒ JZGAME_API 展开成 dllimport (建库侧由 CMake 自动定义的
# JZGame_EXPORTS 决定 dllexport, 见 JZGameGlobal.h)。
target_compile_definitions(${JZ_MODULE_TARGET} PUBLIC JZGAME_DYNAMIC)

# Debug 产物 JZGamed.dll, 与 JZFrameworkd.dll 同一套命名习惯 (可同前缀共存)。
set_target_properties(${JZ_MODULE_TARGET} PROPERTIES DEBUG_POSTFIX "d")

# 运行期落点: 与 JZFramework 一样拷到两个 exe 目录 (Windows 没有 RPATH)。
# ⚠ add_custom_command(TARGET ...) 必须在本 target 所在目录调用 —— 本文件就是在这里
#   被 include 的(根 CMakeLists 作用域), 满足要求。
foreach(_jzg_dir "${CMAKE_BINARY_DIR}/test/bin/$<CONFIG>"
                 "${CMAKE_BINARY_DIR}/binary/bin/$<CONFIG>")
    add_custom_command(TARGET ${JZ_MODULE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_jzg_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:${JZ_MODULE_TARGET}>" "${_jzg_dir}"
        COMMENT "Copy JZGame runtime into ${_jzg_dir}")
endforeach()
unset(_jzg_dir)
