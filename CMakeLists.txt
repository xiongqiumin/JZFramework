cmake_minimum_required(VERSION 3.10)
project(JZFramework)

set(CMAKE_CXX_STANDARD 14)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZNet JZAgent JZBle JZCamera JZCoroutine JZCrashReport
    JZCv JZFrp JZGpio JZHttpServer JZJSEngine JZJson JZWidget JZMcpServer JZModbus JZNode
    JZOTA JZSensor JZSerialPort JZWifi JZTest)

add_subdirectory(demo)
add_subdirectory(test)
add_subdirectory(tools)
add_subdirectory(binary)
