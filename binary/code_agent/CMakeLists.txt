
set(CODE_AGENT_SOURCES
    main.cpp
    MainWindow.h
    MainWindow.cpp
    ChatView.h
    ChatView.cpp
    JZCodeTools.h
    JZCodeTools.cpp
    JZCodeAgent.h
    JZCodeAgent.cpp
    JZEngineRuntime.h
    JZEngineRuntime.cpp
)

add_executable(code_agent ${CODE_AGENT_SOURCES})
target_link_libraries(code_agent PRIVATE JZFramework)

add_subdirectory(runtime)
add_subdirectory(test)