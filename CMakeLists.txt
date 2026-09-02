cmake_minimum_required(VERSION 3.10)
project(JZFramework C CXX ASM)

set(CMAKE_CXX_STANDARD 14)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZNet JZAgent JZAudio JZBle JZCamera JZCoroutine JZCrashReport
    JZCv JZFrp JZGpio JZHttpServer JZJSEngine JZJson JZMcpServer JZModbus JZNode
    JZOTA JZSensor JZSerialPort JZWifi JZTest JZSimulator JZWidget)
target_compile_definitions(JZFramework PRIVATE MG_TLS=MG_TLS_BUILTIN)

option(BUILD_TEST "Build the JZFramework test suite" ON)

add_subdirectory(demo)
if(BUILD_TEST)
    add_subdirectory(test)
endif()
add_subdirectory(tools)
add_subdirectory(binary)
