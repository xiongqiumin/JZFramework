#ifndef JZ_OBJECT_H_
#define JZ_OBJECT_H_

#include <vector>
#include <string>
#include <map>
#include <memory>
#include "JZEvent.h"
#include "JZGlobal.h"
#include "JZSignal.h"
#include "JZObjectPointer.h"

class JZThread;
class JZApplication;

class JZSignalDestory : public JZSignal<JZObject*>
{
public:
    JZSignalDestory(JZObject *object);

    void emitDestory();
};

//JZObjectDeleteEvent
class JZObject;
class JZObjectDeleteEvent : public JZEvent
{
public:
    JZObjectDeleteEvent();
    ~JZObjectDeleteEvent();

    JZObjectPointer object;
};

//JZObjectMeta
class JZObjectMeta
{
public:
    JZObjectMeta(const std::string& className, const JZObjectMeta* parentMeta);
    bool isChildOf(const JZObjectMeta* target) const;

    std::string name;
    const JZObjectMeta* parent;
};

#define JZ_OBJECT_DECLARE(name)  \
    private: static JZObjectMeta staticMeta;          \
    public:  static const JZObjectMeta *metaObject(); \
             virtual const JZObjectMeta* meta() const override;

#define JZ_OBJECT_IMPL(name,parent) \
    JZObjectMeta name::staticMeta(#name, parent::metaObject());   \
    const JZObjectMeta *name::metaObject() { return &name::staticMeta; }  \
    const JZObjectMeta *name::meta() const { return name::metaObject(); }

//JZObject
class JZVariant;
class JZObject
{
public:
    static const JZObjectMeta* metaObject();

    JZObject(JZObject *parent = nullptr);
    virtual ~JZObject();

    JZObjectPointer guardPointer();
    std::string className() const;
    virtual const JZObjectMeta* meta() const;

    void setObjectName(const std::string &name);
    std::string objectName() const;

    bool isBlockSignals() const;
    void blockSignals(bool flag);

    void moveToThread(JZThread *thread);
    JZThread *thread() const;

    virtual void setProperty(const std::string &name, const JZVariant &varaint);
    virtual JZVariant property(const std::string &name) const;

    void setParent(JZObject *parent);
    JZObject * parent() const;    
    const std::vector<JZObject *> &children() const;

    void deleteLater();

jz_signals:
    JZSignalDestory sigDestroyed;

protected:
    JZ_DISABLE_COPY(JZObject)

    friend JZThread;
    friend JZApplication;
    friend JZBaseSignal;

    struct MainThreadInit{};
    JZObject(MainThreadInit);

    int startTimer(int interval);
    void killTimer(int id);    

    virtual bool event(JZEvent *e);
    virtual void timerEvent(JZTimerEvent *event);
    virtual void customEvent(JZEvent *event);    

    void addChild(JZObject *child);
    void removeChild(JZObject *child);
    bool hasChild(JZObject* child) const;

    JZObject *sender();

    std::string* m_name = nullptr;       // 惰性: 未命名时为 nullptr
    JZObject *m_parent = nullptr;
    JZThread *m_thread = nullptr;
    bool m_blockSignals = false;
    std::vector<JZObject*> m_childs;
    std::shared_ptr<JZObjectPointerData> m_data;
    std::unique_ptr<std::map<std::string, JZVariant>> m_properties;

private:
    static JZObjectMeta staticMeta;
};

template<class T>
T jzobject_cast(JZObject* obj)
{
    using RawType = std::remove_pointer_t<T>;
    if (obj->meta()->isChildOf(RawType::metaObject())) {
        return static_cast<T>(obj);
    }
    else {
        return nullptr;
    }
}

#endif