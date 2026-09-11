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

    int exec();
    void exit(int code = 0);
    void quit();
    void processEvents();

    bool isRunning() const;
    int code();

protected:    
    bool m_running;
    int m_code;
};









#endif // !JZ_EVENT_LOOP
