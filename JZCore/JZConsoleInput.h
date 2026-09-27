
// JZConsoleInput.h
#pragma once
#include <atomic>
#include <thread>
#include <iostream>
#include <string>
#include "JZObject.h"

class JZFRAMEWORK_API JZConsoleInput : public JZObject
{
public:
    explicit JZConsoleInput(JZObject* parent = nullptr);
    ~JZConsoleInput();
    
    void start();
    void stop();
    void setPrompt(const std::string& prompt);
    bool isRunning() const;
    
jz_signals:
    JZSigString sigReceived;
    
private:
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::string m_prompt;
};