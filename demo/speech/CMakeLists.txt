# demo/speech: 语音管线 demo (Preprocess -> VAD -> KWS -> ASR -> LLM -> TTS)
set(SPEECH_DEMO_SOURCES main.cpp
    JZSpeech.h
    JZSpeech.cpp
    JZAec.h
    JZAec.cpp)

add_executable(speech_demo ${SPEECH_DEMO_SOURCES})
target_link_libraries(speech_demo PRIVATE JZFramework)
