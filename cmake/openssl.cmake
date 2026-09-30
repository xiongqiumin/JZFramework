# OpenSSL 配置：让 mongoose (MG_TLS) 使用系统 OpenSSL 而非内置实现。
#
# 依赖：机器上需安装 OpenSSL 开发包，约定布局：
#   <OPENSSL_ROOT_DIR>/include/openssl/*.h   (头文件)
#   <OPENSSL_ROOT_DIR>/lib/libssl.lib        (导入库, MinGW 风格布局)
#   <OPENSSL_ROOT_DIR>/lib/libcrypto.lib
# OpenSSL 3.x 官方安装包(MSVC)的导入库在 <root>/lib/VC/x64/<RT>/ 下，见下方探测。
# 运行时还需对应 DLL（libssl-3-x64.dll / libcrypto-3-x64.dll，在 <root>/bin/）在 PATH 上。
# 可用 -DOPENSSL_ROOT_DIR=<path> 覆盖自动探测。

if(NOT DEFINED OPENSSL_ROOT_DIR)
    if(WIN32)
        # 候选根目录，按优先级取第一个"看起来像开发包"的（include + lib + bin）
        set(_ossl_candidate_roots
            "C:/Users/7052/Desktop/work/lib/openssl"   # 本机 lib 仓库: Win64OpenSSL-3_6_4.exe 静默解压到此
            "E:/libs/openssl"                          # 旧约定路径
            "C:/Program Files/OpenSSL-Win64"           # 默认安装路径
        )
        foreach(_root IN LISTS _ossl_candidate_roots)
            if(EXISTS "${_root}/include/openssl/opensslv.h")
                set(OPENSSL_ROOT_DIR "${_root}")
                break()
            endif()
        endforeach()
        if(NOT DEFINED OPENSSL_ROOT_DIR)
            set(OPENSSL_ROOT_DIR "E:/libs/openssl")
        endif()
    endif()
endif()

# 注：FindOpenSSL 自己会搜 <root>/lib/ 与 <root>/lib/VC/x64/MD/（MSVC 布局），
# 所以这里只需要定根目录，不需要手工定位 libssl.lib/libcrypto.lib。

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
