#include "JZNode.h"
#include "JZStd.h"

//JZNode
JZNode::JZNode()
{
    m_flow = nullptr;
}

JZNode::~JZNode()
{
}

void JZNode::setName(std::string name)
{
    m_name = name;
}

std::string JZNode::name()
{
    return m_name;
}

//JZNodeSequence
JZNodeSequence::JZNodeSequence()
{
    next = nullptr;
}

//JZNodeRoot
void JZNodeRoot::run()
{
    m_flow->gotoNext(next);
}

//JZNodeBranch
void JZNodeBranch::run()
{
    JZVariant result = m_flow->getVariable(condition);
    JZ_ASSERT(result.typeId() == JZMetaType::Bool);

    auto flag = result.toBool();
    if(flag)
        m_flow->gotoNext(nextTrue);
    else
        m_flow->gotoNext(nextFalse);
}

//JZNodeFunction
void JZNodeFunction::run()
{
    function();
    m_flow->gotoNext(next);
}

//JZNodeAsyncFunction
void JZNodeAsyncFunction::run()
{
    function(this);
}

void JZNodeAsyncFunction::resolve()
{
    m_flow->gotoNext(next);
}

void JZNodeAsyncFunction::reject(std::string msg)
{
    m_flow->raiseException(msg);
}

//JZNodeAsyncFunctionRace
JZNodeAsyncFunctionRace::JZNodeAsyncFunctionRace()
{
    m_taskId = 0;
    m_finished = false;
}

void JZNodeAsyncFunctionRace::run()
{
    JZ_ASSERT(m_functionList.size() > 0);
    
    m_finished = false;
    m_waitIds.clear();

    for (auto &func : m_functionList)
    {
        int task_id = m_taskId++;
        func(task_id, this);
        m_waitIds.push_back(m_taskId);
    }
}

void JZNodeAsyncFunctionRace::addFunction(std::function<void(int id, JZNodeAsyncFunctionRace *async)> func)
{
    m_functionList.push_back(func);
}

void JZNodeAsyncFunctionRace::resolve(int id)
{
    if (m_finished || !vec_contains(m_waitIds, id))
        return;
    
    m_finished = true;
    m_waitIds.clear();
    m_flow->gotoNext(next);
}

void JZNodeAsyncFunctionRace::reject(int id, std::string errorMsg)
{
    if (m_finished || !vec_contains(m_waitIds, id))
        return;
    
    m_finished = true;
    m_waitIds.clear();
    m_flow->raiseException(errorMsg);
}

//JZNodeAsyncFunctionAll
JZNodeAsyncFunctionAll::JZNodeAsyncFunctionAll()
{
    m_taskId = 0;
    m_finished = false;
}

void JZNodeAsyncFunctionAll::run()
{
    JZ_ASSERT(m_functionList.size() > 0);

    m_finished = false;
    m_waitIds.clear();
    for (auto &func : m_functionList)
    {
        int task_id = m_taskId++;
        func(task_id, this);
        m_waitIds.push_back(task_id);
    }
}

void JZNodeAsyncFunctionAll::addFunction(std::function<void(int id, JZNodeAsyncFunctionAll *async)> func)
{
    m_functionList.push_back(func);
}

// 某个任务成功了
void JZNodeAsyncFunctionAll::resolve(int id)
{
    // 已经结束（失败/完成），直接忽略
    if (m_finished)
        return;

    int idx = vec_indexOf(m_waitIds, id);
    if (idx == -1)
        return;

    // 移除完成的任务
    vec_removeAt(m_waitIds, idx);

    // 全部成功 → 进入下一步
    if (m_waitIds.empty())
    {
        m_finished = true;
        m_flow->gotoNext(next);
    }
}

// 某个任务失败了 → 整体立刻失败
void JZNodeAsyncFunctionAll::reject(int id, std::string errorMsg)
{
    if (m_finished)
        return;
    if (!vec_contains(m_waitIds, id))
        return;

    // 核心：标记结束，阻止所有后续回调
    m_finished = true;

    // 清空等待列表
    m_waitIds.clear();

    // 立即整体失败
    m_flow->raiseException(errorMsg);
}

//JZNodeFlow
JZNodeFlow::JZNodeFlow() 
    : sigFinished(this)
{
    m_root = new JZNodeRoot();
    m_root->m_flow = this;
    m_current = m_root;
    m_isRunning = false;
    m_nodes.push_back(m_root);
}

JZNodeFlow::~JZNodeFlow()
{
    for (auto node : m_nodes)
    {
        delete node;
    }
    m_nodes.clear();
    m_variables.clear();
}

void JZNodeFlow::start()
{
    JZ_ASSERT(!m_isRunning);
    m_isRunning = true;
    gotoNext(m_root);
}

void JZNodeFlow::restart()
{
    m_lastError.clear();
    m_variables.clear();
    m_isRunning = false;
    m_current = m_root;
    gotoNext(m_root);
}

JZNodeRoot *JZNodeFlow::root()
{
    return m_root;
}

void JZNodeFlow::addNode(JZNode *node)
{
    JZ_ASSERT(node->m_flow == nullptr);

    node->m_flow = this;
    m_nodes.push_back(node);
}

void JZNodeFlow::gotoNext(JZNode *node)
{
    m_current = node;
    if(m_current)
        m_current->run();
    else
    {
        sigFinished.emit();
    }
}

void JZNodeFlow::raiseException(std::string msg)
{
    m_lastError = msg;
    sigFinished.emit();
}

std::string JZNodeFlow::getLastError()
{
    return m_lastError;
}

bool JZNodeFlow::hasVariable(const std::string &key)
{
    return map_contains(m_variables,key);
}

void JZNodeFlow::setVariable(const std::string &key, const JZVariant &value)
{
    m_variables[key] = value;
}

JZVariant JZNodeFlow::getVariable(const std::string &key)
{
    return map_value(m_variables, key);
}

bool JZNodeFlow::isSuccess()
{
    return m_lastError.empty();
}

bool JZNodeFlow::isFinished()
{
    return !m_isRunning;
}
