#include "JZStd.h"
#include "JZObject.h"
#include "JZThread.h"
#include "JZApplication.h"
#include "JZTx.h"
#include "JZDebug.h"

JZSignalDestory::JZSignalDestory(JZObject *object)
    :JZSignal<JZObject*>(object)
{
}

void JZSignalDestory::emitDestory()
{
    if (!m_connects) 
        return;

    for (int i = 0; i < (int)m_connects->size(); i++)
    {
        (*m_connects)[i]->type = JZ::DirectConnection;
    }
    emit(m_object);
}

//JZObjectDeleteEvent
JZObjectDeleteEvent::JZObjectDeleteEvent()
    :JZEvent(JZEvent_deleteObject)
{
}
JZObjectDeleteEvent::~JZObjectDeleteEvent()
{
}

//JZObjectMeta
JZObjectMeta::JZObjectMeta(const std::string& className, const JZObjectMeta* parentMeta)
    : name(className), parent(parentMeta)
{
}

bool JZObjectMeta::isChildOf(const JZObjectMeta* target) const 
{
    const JZObjectMeta* cur = this;
    while (cur)
    {
        if (cur == target) 
            return true;
        
        cur = cur->parent;
    }
    return false;
}

//JZObject
JZObjectMeta JZObject::staticMeta("JZObject", nullptr);
const JZObjectMeta* JZObject::metaObject() 
{ 
    return &JZObject::staticMeta; 
}

JZObject::JZObject(JZObject *parent)
    :sigDestroyed(this)
{
    m_thread = jzApp->currentThread();
    setParent(parent); 

    m_data = std::make_shared<JZObjectPointerData>();
    m_data->object = this;
}

JZObject::JZObject(MainThreadInit)
    :sigDestroyed(this)
{
    m_data = std::make_shared<JZObjectPointerData>();
    m_data->object = this;
}

JZObject::~JZObject()
{
    clearGuardPointer();    

    if(m_parent)
        m_parent->removeChild(this);

    // 清理子对象
    std::vector<JZObject*> childs = m_childs;
    for (int i = 0; i < (int)childs.size(); i++)
        delete childs[i];
    m_childs.clear();
    
    // JZThread 统一管理定时器，取消本对象所有定时器
    if (m_thread)
        m_thread->cancelObjectTimers(this);

    m_thread = nullptr;
    m_parent = nullptr;

    // 惰性 name 清理
    if(m_name)
    {
        delete m_name;
        m_name = nullptr;
    }

    sigDestroyed.emitDestory();
}

void JZObject::clearGuardPointer()
{
    if (m_thread){
        JZ_ASSERT(m_thread->id() == JZThread::currentThreadId());
    }

    if(!m_data->object)
        return;

    if(jzApp)
    {
        jzApp->globalLock();
        m_data->object = nullptr;
        jzApp->globalUnlock();        
    }
}

JZObjectPointer JZObject::guardPointer()
{
    JZObjectPointer ptr;
    ptr.data = m_data;
    return ptr;
}

const JZObjectMeta* JZObject::meta() const
{
    return JZObject::metaObject();
}

std::string JZObject::className() const
{
    return meta()->name;
}

bool JZObject::inherits(const char* className) const
{
    if (!className)
        return false;
    const JZObjectMeta* cur = meta();
    while (cur)
    {
        if (cur->name == className)
            return true;
        cur = cur->parent;
    }
    return false;
}

void JZObject::deleteLater()
{
    JZObjectDeleteEvent *event = new JZObjectDeleteEvent();
    event->object = this->guardPointer();

    JZEventInfo event_info(this->thread(), event);
    this->thread()->postEvent(event_info);
}

void JZObject::setObjectName(const std::string &name)
{
    if (name.empty()) {
        delete m_name;
        m_name = nullptr;
    } else {
        if (!m_name) m_name = new std::string();
        *m_name = name;
    }
}

std::string JZObject::objectName() const
{
    return m_name ? *m_name : std::string();
}

bool JZObject::isBlockSignals() const
{
    return m_blockSignals;
}

void JZObject::blockSignals(bool flag)
{
    m_blockSignals = flag;
}

void JZObject::moveToThread(JZThread *thread)
{
    JZ_ASSERT(thread && m_thread == jzApp->currentThread());
    
    m_thread = thread;
    for (int i = 0; i < (int)m_childs.size(); i++)
    {
        m_childs[i]->moveToThread(m_thread);
    }
}

JZThread *JZObject::thread() const
{
    return m_thread;
}

void JZObject::setProperty(const std::string &name, const JZVariant &varaint)
{
    if (!m_properties) 
        m_properties.reset(new std::map<std::string, JZVariant>());
    
    if (varaint.isValid())
        (*m_properties)[name] = varaint;
    else
        map_remove(*m_properties, name);
}

JZVariant JZObject::property(const std::string &name) const
{
    if (!m_properties) 
        return JZVariant::None();
    
    auto it = m_properties->find(name);
    if (it != m_properties->end())
        return it->second;
    
    return JZVariant::None();
}

void JZObject::setParent(JZObject *parent)
{
    if (m_parent)
    {
        m_parent->removeChild(this);
        m_parent = nullptr;
    }
        
    if (parent)
    {
        parent->addChild(this);
        m_parent = parent;
    }
}

JZObject *JZObject::parent() const
{
    return m_parent;
}

const std::vector<JZObject *> &JZObject::children() const
{
    return m_childs;
}

bool JZObject::event(JZEvent *e)
{
    JZTX_FUNC_TRACE
    if (e->type() == JZEvent_timer)
    {
        JZTimerEvent *time_event = (JZTimerEvent*)e;
        timerEvent(time_event);
    }
    else if (e->type() >= JZEvent_user)
    {
        customEvent(e);
    }

    return true;
}

void JZObject::timerEvent(JZTimerEvent *event)
{

}

void JZObject::customEvent(JZEvent *event)
{

}

JZObject *JZObject::sender()
{
    auto &q = m_thread->m_senderQueue;
    return q.empty() ? nullptr : q.back();
}

void JZObject::addChild(JZObject *obj)
{
    JZ_ASSERT(obj != this && !obj->hasChild(this) && !hasChild(obj));
    m_childs.push_back(obj);
}

void JZObject::removeChild(JZObject *obj)
{
    vec_removeAll(m_childs, obj);
}

bool JZObject::hasChild(JZObject* child) const
{
    auto parent = child->parent();
    while (parent)
    {
        if (parent == this)
            return true;

        parent = parent->parent();
    }
    return false;
}

int JZObject::startTimer(int interval)
{
    JZ_ASSERT(thread() == jzApp->currentThread());
    return thread()->addTimer(this, interval);
}

void JZObject::killTimer(int id)
{
    JZ_ASSERT(thread() == jzApp->currentThread());
    thread()->removeTimer(this, id);
}

JZDebug operator<<(JZDebug d, const JZObject& obj)
{
    d << "JZObject(" << obj.className();
    std::string name = obj.objectName();
    if (!name.empty())
        d << ", " << name;
    d << ")";
    return d;
}