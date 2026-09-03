
set(CODE_AGENT_SOURCES
    main.cpp
    MainWindow.h
    MainWindow.cpp
    JZCodeTools.h
    JZCodeTools.cpp
    JZCodeAgent.h
    JZCodeAgent.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)