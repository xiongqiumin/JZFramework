# ==================== 生成 jz_version.h ====================
# 在配置阶段根据基础版本号 + Git 信息生成 jz_version.h。
# 用法: 在顶层或 JZFramework.cmake 中 include 本文件, 生成头文件输出到
#       ${JZ_VERSION_OUTPUT_DIR} (默认 ${CMAKE_BINARY_DIR}/generated),
#       并挂载到 JZFramework 的 PUBLIC 包含目录。
#
# 提供宏:
#   JZ_GIT_VERSION / JZ_GIT_COMMIT / JZ_GIT_BRANCH / JZ_GIT_DIRTY / JZ_BUILD_TIME

# ---- Git 信息 ----
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        OUTPUT_VARIABLE JZ_GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${JZFRAMEWORK_ROOT_DIR})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        OUTPUT_VARIABLE JZ_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${JZFRAMEWORK_ROOT_DIR})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        OUTPUT_VARIABLE JZ_GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${JZFRAMEWORK_ROOT_DIR})
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        OUTPUT_VARIABLE _jz_git_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        WORKING_DIRECTORY ${JZFRAMEWORK_ROOT_DIR})
    if(_jz_git_status STREQUAL "")
        set(JZ_GIT_DIRTY 0)
    else()
        set(JZ_GIT_DIRTY 1)
    endif()
else()
    set(JZ_GIT_VERSION "unknown")
    set(JZ_GIT_COMMIT "unknown")
    set(JZ_GIT_BRANCH "unknown")
    set(JZ_GIT_DIRTY 0)
endif()

# Git 获取失败时的兜底
if(NOT JZ_GIT_VERSION)
    set(JZ_GIT_VERSION "unknown")
endif()
if(NOT JZ_GIT_COMMIT)
    set(JZ_GIT_COMMIT "unknown")
endif()
if(NOT JZ_GIT_BRANCH)
    set(JZ_GIT_BRANCH "unknown")
endif()

# ---- 构建时间 (ISO 8601, UTC) ----
string(TIMESTAMP JZ_BUILD_TIME "%Y-%m-%d %H:%M:%S" UTC)

# ---- 输出目录与文件 ----
if(NOT DEFINED JZ_VERSION_OUTPUT_DIR)
    set(JZ_VERSION_OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated")
endif()
set(JZ_VERSION_HEADER "${JZ_VERSION_OUTPUT_DIR}/jz_version.h")

file(MAKE_DIRECTORY "${JZ_VERSION_OUTPUT_DIR}")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/jz_version.h.in"
    "${JZ_VERSION_HEADER}"
    @ONLY)

message(STATUS "[JZFramework] Generated version header: ${JZ_VERSION_HEADER} (git: ${JZ_GIT_VERSION})")

# ---- 挂载到 JZFramework 包含目录 (供 JZCore 等模块 #include "jz_version.h") ----
if(TARGET JZFramework)
    target_include_directories(JZFramework PUBLIC "${JZ_VERSION_OUTPUT_DIR}")
endif()
