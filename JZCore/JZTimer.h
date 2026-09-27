#ifndef JZ_TIMER_H_
#define JZ_TIMER_H_
#include "JZObject.h"
#include <functional>

class JZFRAMEWORK_API JZTimer : public JZObject
{
public:
    static void singleShot(int msec, std::function<void()> func);
    static void singleShot(int msec, JZObject *object, std::function<void()> func);

    JZTimer(JZObject *parent = nullptr);
    ~JZTimer();

    void start(int ms);
    void stop();
    void restart();

    bool isActive() const;
    bool isSingleShot() const;
    void setSingleShot(bool singleShot);

jz_signals:
    JZSigVoid sigTimeout;

protected:
    virtual void timerEvent(JZTimerEvent *event) override;

    int m_timeId;
    int m_gap;
    bool m_singleShot;
};

#endif // !JZ_TIMER_H_
