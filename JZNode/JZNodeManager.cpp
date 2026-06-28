#include "JZNodeManager.h"

JZNodeManager::JZNodeManager(JZObject *parent) 
    : JZObject(parent), sigFlowFinished(this)
{
}

JZNodeManager::~JZNodeManager()
{
}

void JZNodeManager::start(JZNodeFlow *flow)
{
    flow->setParent(this);
    connect(&flow->sigFinished, this, &JZNodeManager::onFlowFinish);
    flow->start();
}

void JZNodeManager::onFlowFinish()
{
    auto flow = (JZNodeFlow *)sender();
    sigFlowFinished.emit(flow);
}