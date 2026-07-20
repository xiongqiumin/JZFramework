# demo/speech: 语音管线 demo (Preprocess -> VAD -> KWS -> ASR -> LLM -> TTS)
set(DEMO_SPEECH_SOURCES
    main.cpp
    JZSpeechClient.cpp
    JZSpeechProcessor.cpp
    JZSpeechCore.cpp
    JZSpeechDemo.cpp)
convert_to_abs_paths(DEMO_SPEECH_SOURCES)

add_executable(demo_speech ${DEMO_SPEECH_SOURCES})
target_link_libraries(demo_speech PRIVATE JZFramework)
