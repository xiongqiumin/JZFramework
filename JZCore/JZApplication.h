#ifndef JZ_APPLICATION_H_
#define JZ_APPLICATION_H_

#include "JZObject.h"
#include "JZEventLoop.h"
#include "JZThread.h"
#include "JZMutex.h"

class JZApplication : public JZObject
{
public:
    static JZApplication* instance();
    
    JZApplication();
    ~JZApplication();    

    std::string applicationFilePath();
    std::string applicationDirPath();

    JZThread *mainThread();
    JZThread *currentThread();

    void addThread(JZThread *thread);
    void removeThread(JZThread *thread);

    void globalLock();
    void globalUnlock();
    
    int exec();        
    void quit();
    void exit(int code);
    void processEvents();

    void sendEvent(JZObject *object, JZEvent *event);
    void postEvent(JZObject *object, JZEvent *event);
    void postEventAndWait(JZObject *object, JZEvent *event);    

protected:    
    virtual void timerEvent(JZTimerEvent *event) override;

    JZThread *m_mainThread;

    std::vector<JZThread*> m_threads;
    JZMutex m_mutex;
    int m_returnCode;

    int m_lvglTimer = 0;
    int64_t m_lvglTick = 0;
};
extern JZApplication *jzApp;

#endif


