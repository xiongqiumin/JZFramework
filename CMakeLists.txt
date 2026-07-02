cmake_minimum_required(VERSION 3.10)
project(JZFramework)

if (MSVC)    
    add_compile_options(/execution-charset:utf-8)
    add_compile_options(/source-charset:utf-8)
    add_compile_options(/bigobj)
endif()

include(JZFramework.cmake)
jzframework_use_modules(JZCore JZLvgl)
jzframework_build_test()
