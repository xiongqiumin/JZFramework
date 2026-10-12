# JZGame: 2D 游戏控件层 —— 基于 JZWidget 的控件/绘制/键盘/定时器。
# 目前仅支持 Windows 平台 (JZWidget 只在 Windows 下构建; 根 CMakeLists 也只在那里启用本模块)。
#
# 本模块是"普通模块"(编进 JZFramework): 没有外部依赖 (不像 JZEmulator 要外部 DLL),
# 消费方 target_link_libraries(... JZFramework) 即可用, 头文件经
# target_include_directories 暴露 (JZGameView.h)。
if(NOT WIN32)
    return()
endif()

set(JZGame_SOURCES
    JZGameView.h
    JZGameView.cpp
    # 2D 游戏三件套: 碰撞体(AABB) / 精灵(值语义, 弹幕向) / 场景(绘制队列+重叠查询)
    JZBody.h
    JZSprite.h
    JZSprite.cpp
    JZScene.h
    JZScene.cpp
)

convert_to_abs_paths(JZGame_SOURCES)

# 添加到目标
target_sources(${JZ_MODULE_TARGET} PRIVATE ${JZGame_SOURCES})

target_include_directories(${JZ_MODULE_TARGET} PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
