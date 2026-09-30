# ---------------------------------------------------------------------------
# "只在目标不存在时复制" —— 脚本模式执行:
#     ${CMAKE_COMMAND} -Dsrc=<源文件> -Ddst=<目标文件> -P cmake/copy_if_absent.cmake
#
# 用途: 把"测试配置样例"(如 test_llm.json) 放到 exe 同目录给用户改。
# 为什么不用 copy_if_different: 那会在每次构建时把用户改过的那份**覆盖回去** ——
# 而这份文件的全部意义就是"本机自己改"; 所以只在缺失时铺一份, 之后构建不碰它。
# ---------------------------------------------------------------------------
if(NOT DEFINED src OR NOT DEFINED dst)
    message(FATAL_ERROR "copy_if_absent.cmake: 需要 -Dsrc=... -Ddst=...")
endif()

if(EXISTS "${dst}")
    return()   # 已有: 用户可能改过, 不覆盖
endif()

get_filename_component(_dir "${dst}" DIRECTORY)
file(MAKE_DIRECTORY "${_dir}")
get_filename_component(_name "${dst}" NAME)
configure_file("${src}" "${dst}" COPYONLY)   # 按目标名写, 不依赖源文件名
# 消息用英文: 构建日志的编码在这台机器上是 GBK, 中文会变成乱码
message(STATUS "[copy_if_absent] ${_name} was missing, placed a sample copy: ${dst}")
