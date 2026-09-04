add_executable(test_agent test_agent.cpp)
target_link_libraries(test_agent JZFramework)

add_executable(test_mcpclient test_mcpclient.cpp)
target_link_libraries(test_mcpclient JZFramework)

add_executable(test_image test_image.cpp)
target_link_libraries(test_image JZFramework)

add_subdirectory(five_chess)
