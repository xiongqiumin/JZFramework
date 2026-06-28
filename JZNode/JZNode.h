#ifndef JZNODE_H
#define JZNODE_H

#include <functional>
#include "JZVariant.h"
#include "JZObject.h"

class JZNodeFlow;
class JZNode
{
public:
    JZNode();
    virtual ~JZNode();

    void setName(std::string name);
    std::string name();
    virtual void run() = 0;

protected:
    friend JZNodeFlow;

    std::string m_name;
    JZNodeFlow *m_flow;
};

class JZNodeSequence : public JZNode
{
public:
    JZNodeSequence();

    JZNode* next;
};

class JZNodeRoot : public JZNodeSequence
{
public:
    virtual void run() override;
};

class JZNodeBranch : public JZNode 
{
public:
    virtual void run() override;

    std::string condition;
    JZNode* nextTrue;
    JZNode* nextFalse;
};

class JZNodeFunction : public JZNodeSequence 
{
public:
    virtual void run() override;

    std::function<void()> function;
};

class JZNodeAsyncFunction : public JZNodeSequence 
{
public:
    virtual void run() override;
    
    void resolve();
    void reject(std::string errorMsg);

    std::function<void(JZNodeAsyncFunction *async)> function;
};

class JZNodeAsyncFunctionRace : public JZNodeSequence 
{
public:
    JZNodeAsyncFunctionRace();

    virtual void run() override;
    
    void addFunction(std::function<void(int id, JZNodeAsyncFunctionRace *async)> func);
    void resolve(int id);
    void reject(int id, std::string errorMsg);

protected:
    int m_taskId;
    std::vector<int> m_waitIds;
    bool m_finished;

    std::vector<std::function<void(int id, JZNodeAsyncFunctionRace *async)>> m_functionList;
};

class JZNodeAsyncFunctionAll : public JZNodeSequence 
{
public:
    JZNodeAsyncFunctionAll();

    virtual void run() override;
    
    void addFunction(std::function<void(int id, JZNodeAsyncFunctionAll *async)> func);
    void resolve(int id);
    void reject(int id, std::string msg);

protected:
    int m_taskId;
    std::vector<int> m_waitIds;
    bool m_finished;

    std::vector<std::function<void(int id, JZNodeAsyncFunctionAll *async)>> m_functionList;
};

class JZNodeFlow : public JZObject 
{
public:
    JZNodeFlow();
    ~JZNodeFlow();

    void start();
    void restart();

    void addNode(JZNode *node);      //只添加

    JZNodeRoot *root();
    void gotoNext(JZNode *node);

    void raiseException(std::string msg);
    std::string getLastError();
    bool isFinished();
    bool isSuccess();
    
    bool hasVariable(const std::string &key);
    void setVariable(const std::string &key, const JZVariant &value);
    JZVariant getVariable (const std::string &key);

jz_signals: 
    JZSigVoid sigFinished;

protected:
    JZNodeRoot *m_root;
    JZNode *m_current;
    std::vector<JZNode*> m_nodes;

    std::map<std::string, JZVariant> m_variables;
    std::string m_lastError;
    bool m_isRunning;
};

#endif
