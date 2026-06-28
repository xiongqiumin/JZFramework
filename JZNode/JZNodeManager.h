#ifndef JZNODEMANAGER_H
#define JZNODEMANAGER_H

#include "JZNode.h"

class JZNodeManager : public JZObject
{
public:
    JZNodeManager(JZObject *parent = nullptr);
    ~JZNodeManager();

    void start(JZNodeFlow *flow);

jz_signals:
    JZSignal<JZNodeFlow*> sigFlowFinished;

protected:
    void onFlowFinish();

};

#endif