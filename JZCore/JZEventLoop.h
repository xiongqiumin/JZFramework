#ifndef JZ_EVENT_LOOP_H_
#define JZ_EVENT_LOOP_H_

#include <vector>
#include <mutex>
#include "JZObject.h"
#include "JZSemaphore.h"

class JZEventLoop : public JZObject
{
public:
    JZEventLoop();
    ~JZEventLoop();

    void exec();
    void exit();
    void quit();
    void processEvents();

protected:    
    bool m_running;
};









#endif // !JZ_EVENT_LOOP
