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

    bool inherits(const char* className) const;

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

    // ---- 事件过滤 (Qt 风格) ----
    // installEventFilter: 把 filterObj 注册为本对象的过滤器。之后本对象收到任何
    //   事件 (同步 sendEvent / 异步队列 / 定时器) 时, 会先按注册顺序调用每个
    //   过滤器 filterObj->eventFilter(this, event); 任一返回 true 则事件被拦截,
    //   不再进入本对象自身的 event()/xxxEvent()。
    //   - 同一 filter 重复安装被忽略 (列表内只保留一份)。
    //   - 过滤器对象必须活得比本对象久 (或由本对象先析构); filter 先析构会留下
    //     悬垂指针, 使用方须先 removeEventFilter。
    void installEventFilter(JZObject *filterObj);
    void removeEventFilter(JZObject *filterObj);

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
    virtual bool eventFilter(JZObject *watched, JZEvent *event);
    virtual void timerEvent(JZTimerEvent *event);
    virtual void customEvent(JZEvent *event);    

    void addChild(JZObject *child);
    void removeChild(JZObject *child);
    bool hasChild(JZObject* child) const;
    void clearGuardPointer();

    JZObject *sender();

    std::string* m_name = nullptr;       // 惰性: 未命名时为 nullptr
    JZObject *m_parent = nullptr;
    JZThread *m_thread = nullptr;
    bool m_blockSignals = false;
    std::vector<JZObject*> m_childs;
    std::shared_ptr<JZObjectPointerData> m_data;
    std::unique_ptr<std::map<std::string, JZVariant>> m_properties;

private:
    // 依次询问所有事件过滤器 (拷贝遍历, 允许回调中 install/remove); 
    // 任一返回 true 则拦截, 返回 true。仅 JZApplication::sendEvent 与
    // JZThread::notify 这两个"事件进入对象"的统一入口调用。
    bool applyEventFilters(JZEvent *event);

    std::vector<JZObject*> m_eventFilters;

    static JZObjectMeta staticMeta;
};

template<class T>
T jzobject_cast(JZObject* obj)
{
    using RawType = std::remove_pointer_t<T>;
    if (obj && obj->meta()->isChildOf(RawType::metaObject())) {
        return static_cast<T>(obj);
    }
    else {
        return nullptr;
    }
}

JZDebug operator<<(JZDebug d, const JZObject *obj);

#endif