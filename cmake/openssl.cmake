# OpenSSL 配置：让 mongoose (MG_TLS) 使用系统 OpenSSL 而非内置实现。
#
# 依赖：机器上需安装 OpenSSL 开发包，约定布局：
#   <OPENSSL_ROOT_DIR>/include/openssl/*.h   (头文件)
#   <OPENSSL_ROOT_DIR>/lib/libssl.lib        (导入库)
#   <OPENSSL_ROOT_DIR>/lib/libcrypto.lib
# 运行时还需对应 DLL（如 libssl-1_1-x64.dll / libcrypto-1_1-x64.dll）在 PATH 上。
# 可用 -DOPENSSL_ROOT_DIR=<path> 覆盖自动探测。

if(NOT DEFINED OPENSSL_ROOT_DIR)
    if(WIN32)
        # 本机约定： E:\libs\openssl 为完整开发包（include/ + lib/ + bin/）
        if(EXISTS "E:/libs/openssl/lib/libssl.lib")
            set(OPENSSL_ROOT_DIR "E:/libs/openssl")
        elseif(EXISTS "C:/Program Files/OpenSSL-Win64/lib/libssl.lib")
            set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64")
        else()
            set(OPENSSL_ROOT_DIR "E:/libs/openssl")
        endif()
    endif()
endif()

find_package(OpenSSL REQUIRED)

message(STATUS "OpenSSL found: ${OPENSSL_VERSION} (root=${OPENSSL_ROOT_DIR}, include=${OPENSSL_INCLUDE_DIR})")

# 注: 不在这里切换 MG_TLS(默认 MG_TLS_BUILTIN), 仅让 JZCore/crypto 能链接 libcrypto。
# 如需让 mongoose 走 OpenSSL TLS, 在顶层 CMakeLists 自行定义 MG_TLS=MG_TLS_OPENSSL。

# 头文件与链接（优先用 imported target，回退到变量）
# 注意: JZFramework 是 STATIC 库, 用 PUBLIC 才能让链接 JZFramework 的最终可执行文件
# 自动带上 OpenSSL(否则会出现 OpenSSL 符号未定义)。
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    target_link_libraries(JZFramework PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    target_include_directories(JZFramework PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(JZFramework PUBLIC ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY})
endif()
