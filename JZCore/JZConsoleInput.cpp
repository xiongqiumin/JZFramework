
#include "JZConsoleInput.h"
#include <iostream>
#include <string>
#include "JZStd.h"

JZConsoleInput::JZConsoleInput(JZObject* parent)
    :JZObject(parent), sigReceived(this)
{
    m_running = false;
}
    
JZConsoleInput::~JZConsoleInput() 
{ 
    stop(); 
}

void JZConsoleInput::start() 
{
    if (m_running) 
        return;
    
    m_running = true;
#ifdef JZ_OS_NORMAL
    m_thread = std::thread([this]() {
        std::string line;
        while (m_running) 
        {
            if (!m_prompt.empty()) {
                std::cout << str_utf8_to_local(m_prompt) << std::flush;
            }
            std::getline(std::cin, line);
            if (!m_running) 
                break;
            
            line = str_local_to_utf8(line);
            sigReceived.emit(line);
        }
    });
#endif
}

void JZConsoleInput::stop() 
{
    if (!m_running) 
        return;
    
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void JZConsoleInput::setPrompt(const std::string& prompt) 
{ 
    m_prompt = prompt; 
}

bool JZConsoleInput::isRunning() const 
{ 
    return m_running; 
}