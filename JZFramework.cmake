cmake_minimum_required(VERSION 3.12)

# 防止文件被重复 include
include_guard(GLOBAL)

# 创建全局的 JZFramework 库目标（静态库）
add_library(JZFramework STATIC "")

# ==================== 模块依赖关系表 ====================
# 格式: set(MODULE_DEPENDS_<模块名> "依赖模块1;依赖模块2")
set(MODULE_DEPENDS_JZCore "")
set(MODULE_DEPENDS_JZJson "JZCore")
set(MODULE_DEPENDS_JZNet "JZCore")
set(MODULE_DEPENDS_JZFrp "JZNet")
set(MODULE_DEPENDS_JZBle "JZCore")
set(MODULE_DEPENDS_JZWifi "JZCore")
set(MODULE_DEPENDS_JZSerialPort "JZCore")
set(MODULE_DEPENDS_JZGpio "JZCore")
set(MODULE_DEPENDS_JZCoroutine "JZCore;JZSerialPort")
set(MODULE_DEPENDS_JZAgent "JZNet;JZJson")
set(MODULE_DEPENDS_JZJSEngine "JZNet;JZJson")
set(MODULE_DEPENDS_JZLvgl "JZCore")
set(MODULE_DEPENDS_JZSimulator "JZBle;JZNet;JZLvgl;JZGpio")
set(MODULE_DEPENDS_JZCamera "JZCore")
set(MODULE_DEPENDS_JZCrashReport "JZCore;JZNet")
set(MODULE_DEPENDS_JZModbus "JZSerialPort;JZNet")

set(JZFRAMEWORK_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}")
message(STATUS "JZFramework root directory: ${JZFRAMEWORK_ROOT_DIR}")
target_include_directories(JZFramework PUBLIC "${JZFRAMEWORK_ROOT_DIR}")

# 已添加模块的缓存列表（防止重复添加）
set(JZFRAMEWORK_ADDED_MODULES "" CACHE INTERNAL "")

# 辅助函数：相对路径转绝对路径
function(convert_to_abs_paths VAR_NAME)
    set(ABS_PATHS "")
    foreach(PATH ${${VAR_NAME}})
        if(NOT IS_ABSOLUTE "${PATH}")
            get_filename_component(ABS_PATH "${PATH}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
            list(APPEND ABS_PATHS "${ABS_PATH}")
        else()
            list(APPEND ABS_PATHS "${PATH}")
        endif()
    endforeach()
    set(${VAR_NAME} "${ABS_PATHS}" PARENT_SCOPE)
endfunction()

# ==================== 递归添加模块（内部函数） ====================
function(_jz_declare_model MODULE_NAME)
	# 如果模块已经添加过，则直接返回
    if(${MODULE_NAME} IN_LIST JZFRAMEWORK_ADDED_MODULES)
        return()
    endif()
	
	target_compile_definitions(${JZ_MODULE_TARGET} PUBLIC HAS_${MODULE_NAME})

    # 先递归添加依赖模块
    set(DEPENDS_VAR "MODULE_DEPENDS_${MODULE_NAME}")
    if(DEFINED ${DEPENDS_VAR})
        foreach(DEP ${${DEPENDS_VAR}})
            _jz_declare_model(${DEP})
        endforeach()
    endif()
endfunction()

function(_jz_add_module MODULE_NAME)
    # 如果模块已经添加过，则直接返回
    if(${MODULE_NAME} IN_LIST JZFRAMEWORK_ADDED_MODULES)
        return()
    endif()

    # 先递归添加依赖模块
    set(DEPENDS_VAR "MODULE_DEPENDS_${MODULE_NAME}")
    if(DEFINED ${DEPENDS_VAR})
        foreach(DEP ${${DEPENDS_VAR}})
            _jz_add_module(${DEP})
        endforeach()
    endif()

    # 模块所在目录
    set(MODULE_DIR "${JZFRAMEWORK_ROOT_DIR}/${MODULE_NAME}")
    if(NOT EXISTS "${MODULE_DIR}")
        message(FATAL_ERROR "JZFramework module ${MODULE_NAME} not found at ${MODULE_DIR}")
    endif()

    # 模块的 cmake 配置文件路径
    set(MODULE_CMAKE_FILE "${MODULE_DIR}/${MODULE_NAME}.cmake")
    if(NOT EXISTS "${MODULE_CMAKE_FILE}")
        message(FATAL_ERROR "Module ${MODULE_NAME} is missing its config file: ${MODULE_CMAKE_FILE}")
    endif()

    # 传递全局目标名给模块配置文件
    include("${MODULE_CMAKE_FILE}")

    # 标记模块已添加
    list(APPEND JZFRAMEWORK_ADDED_MODULES ${MODULE_NAME})
    set(JZFRAMEWORK_ADDED_MODULES "${JZFRAMEWORK_ADDED_MODULES}" CACHE INTERNAL "")
endfunction()

macro(jz_module_exists MODULE_NAME RESULT_VAR)
	set(${RESULT_VAR} OFF)
	if(${MODULE_NAME} IN_LIST JZFRAMEWORK_ADDED_MODULES)
		set(${RESULT_VAR} ON)
    endif()
endmacro()

# ==================== 用户接口：设置要启用的模块 ====================
macro(jzframework_use_modules)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SINGLE_MODULES)
    cmake_parse_arguments(JZ_USE "" "" "${multiValueArgs}" ${ARGN})
	
    # 普通模块 = 所有未被解析的参数
    set(modules ${JZ_USE_UNPARSED_ARGUMENTS})
    # 单例模块 = 显式指定的 SINGLE_MODULES 后的参数
    set(single_modules ${JZ_USE_SINGLE_MODULES})

    # 处理普通模块
    set(JZ_MODULE_TARGET JZFramework)
    if(NOT MSVC)
        target_compile_options(${JZ_MODULE_TARGET} PRIVATE -Wno-psabi)
    endif()
    unset(JZFRAMEWORK_ADDED_MODULES CACHE)
	
	foreach(MODULE ${modules})
        _jz_declare_model(${MODULE})
    endforeach()
    foreach(MODULE ${modules})
        _jz_add_module(${MODULE})
    endforeach()
    unset(JZ_MODULE_TARGET)

    # 处理单例模块（可选）
    foreach(s ${single_modules})
        add_library(${s} STATIC "")
        set(JZ_MODULE_TARGET ${s})
        include(${JZFRAMEWORK_ROOT_DIR}/${s}/${s}.cmake)
        target_link_libraries(${s} PRIVATE JZFramework)
    endforeach()

    message(STATUS "JZFramework enabled modules: ${JZFRAMEWORK_ADDED_MODULES}")
endmacro()

function(jzframework_use_config TEMPLATE_FILE)
    
    # 模板文件路径：当前调用CMakeLists.txt所在目录
    set(SRC_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/${TEMPLATE_FILE}")
    # 独立 generated 目录
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    # 固定输出文件名
    set(OUTPUT_HEADER "${GEN_DIR}/JZFrameworkConfig.h")

    if(NOT EXISTS "${SRC_TEMPLATE}")
        message(FATAL_ERROR "Template file not found: ${SRC_TEMPLATE}")
    endif()

    file(MAKE_DIRECTORY "${GEN_DIR}")

    configure_file("${SRC_TEMPLATE}" "${OUTPUT_HEADER}" @ONLY)

    # 给 JZFramework 挂载头文件搜索路径
    target_include_directories(JZFramework PUBLIC "${GEN_DIR}")

    # 模板修改自动重新生成
    target_sources(JZFramework PRIVATE "${SRC_TEMPLATE}")

    message(STATUS "[JZFramework] Generate config header: ${OUTPUT_HEADER}")
endfunction()

# ==================== 资源编译: .qrc → .cpp 静态库 ====================
function(jz_add_resources TARGET_NAME)
    set(JZ_RES_PY "${JZFRAMEWORK_ROOT_DIR}/tools/jz_resources.py")
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/${TARGET_NAME}")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(OUT_CPPS "")

    foreach(QRC_FILE ${ARGN})
        get_filename_component(QRC_NAME "${QRC_FILE}" NAME_WE)
        set(OUT_CPP "${GEN_DIR}/${QRC_NAME}_resources.cpp")

        if(NOT IS_ABSOLUTE "${QRC_FILE}")
            set(QRC_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${QRC_FILE}")
        else()
            set(QRC_ABS "${QRC_FILE}")
        endif()

        set(DEPFILE "${OUT_CPP}.d")

        # 解析 .qrc 提取资源文件，显式声明为 DEPENDS（兼容 MSVC 生成器）
        file(READ "${QRC_ABS}" QRC_CONTENT)
        string(REGEX MATCHALL ">[ \t]*([^<>\n]+)[ \t]*<" QRC_MATCHES "${QRC_CONTENT}")
        set(QRC_RESOURCES "")
        get_filename_component(QRC_DIR "${QRC_ABS}" DIRECTORY)
        foreach(MATCH ${QRC_MATCHES})
            # 剥离 > < 和空白
            string(REGEX REPLACE "^>[ \t]*" "" RES "${MATCH}")
            string(REGEX REPLACE "[ \t]*<$" "" RES "${RES}")
            if(EXISTS "${QRC_DIR}/${RES}")
                list(APPEND QRC_RESOURCES "${QRC_DIR}/${RES}")
            endif()
        endforeach()

        add_custom_command(
            OUTPUT "${OUT_CPP}"
            COMMAND python "${JZ_RES_PY}" "${QRC_ABS}" -o "${OUT_CPP}" -d "${DEPFILE}"
            DEPENDS "${QRC_ABS}" "${JZ_RES_PY}" ${QRC_RESOURCES}
            DEPFILE "${DEPFILE}"
            COMMENT "Generating resources: ${QRC_FILE} -> ${OUT_CPP}"
        )

        list(APPEND OUT_CPPS "${OUT_CPP}")
        message(STATUS "[JZFramework] Resource: ${QRC_FILE} (${QRC_RESOURCES}) -> ${OUT_CPP}")
    endforeach()

    add_library(${TARGET_NAME} OBJECT ${OUT_CPPS})
    target_link_libraries(${TARGET_NAME} PUBLIC JZFramework)
endfunction()

macro(jzframework_build_binary)
	include(${JZFRAMEWORK_ROOT_DIR}/binary/binary.cmake)
endmacro()

macro(jzframework_build_test)
	include(${JZFRAMEWORK_ROOT_DIR}/test/test.cmake)
endmacro()

macro(jzframework_build_simulator)
	include(${JZFRAMEWORK_ROOT_DIR}/simulator/simulator.cmake)
endmacro()