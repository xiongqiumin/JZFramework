#ifndef JZ_JS_LIST_WIDGET_H_
#define JZ_JS_LIST_WIDGET_H_

#include "JZListWidget.h"
#include "JZJSType.h"
#include "JZJSWidgetModule.h"   // JSWidgetClass<Class> (逐虚函数事件转发)

// JZListWidget 的 JS 侧包装(镜像)类。命名约定与理由见 JZJSWidget.h 顶部说明
// (签名中出现 JS 指针类型的方法统一以 Js 结尾; JS 侧看到的名字由绑定层 def() 决定)。
//
// 这里要解决的是**数据项的所有权/失效**问题:
//   JZListWidgetItem 不是 JZObject(没有守卫指针), 所以只能用 JSRawPointer<T>,
//   而它对 delete 无感知 —— 失效必须有人显式 clear()。
// 做法与 JZWidget.layout() 的"布局包装缓存"同一思路(见 JZJSWidget.cpp), 只是"每项一份"开关,
// 且**按项指针索引而不是下标**:
//   插入/删除会让下标整体错位, 若按下标存开关, 一次 insert 就会把还活着的项的包装误失效。
//   项指针在本列表内是稳定的(见 JZListWidget 的 unique_ptr 存储), 所以用它做键。
class JZJSListWidget : public JSWidgetClass<JZListWidget>
{
    JZ_OBJECT_DECLARE(JZJSListWidget)

public:
    explicit JZJSListWidget(JZWidget* parent = nullptr);
    ~JZJSListWidget() override;

    // ---- 查询 (借用包装: cppOwner=true, 由列表持有) ----
    jsbind::JSRawPointer<JZListWidgetItem> itemJs(int index) const;   // 越界返回空包装
    jsbind::JSRawPointer<JZListWidgetItem> currentItemJs() const;

    // ---- 追加/插入 ----
    jsbind::JSRawPointer<JZListWidgetItem> appendItemJs(const std::string& text);
    jsbind::JSRawPointer<JZListWidgetItem> insertItemJs(int index, const std::string& text);

    // 接管 JS 创建的项(/Transfer/, 对应 list.appendItem(new Item())):
    // 由本列表负责 delete, 传入的包装被翻转为"非拥有", JS GC 不再重复释放。
    jsbind::JSRawPointer<JZListWidgetItem> appendItemJs(jsbind::JSRawPointer<JZListWidgetItem>& item);
    jsbind::JSRawPointer<JZListWidgetItem> insertItemJs(int index, jsbind::JSRawPointer<JZListWidgetItem>& item);

    // ---- 摘除 (必须走这里, 否则 JS 侧持有的包装会悬垂) ----
    void removeItemJs(int index);                                     // 摘除并删除
    jsbind::JSRawPointer<JZListWidgetItem> takeItemJs(int index);     // 摘除并交回 JS 所有权(/TransferBack/)
    void clearJs();                                                   // 先失效再清空

protected:
    struct ItemSwitch
    {
        JZListWidgetItem* item = nullptr;
        jsbind::JSRawPointer<JZListWidgetItem> sw;
    };

    // 按项指针取/建失效开关(新建的是"借用")。
    jsbind::JSRawPointer<JZListWidgetItem>& switchFor(JZListWidgetItem* item) const;
    // 用 src 的包装作为开关(共享同一 PointInfo), 使失效能传播回 JS 侧持有的那份。
    jsbind::JSRawPointer<JZListWidgetItem> adoptSwitch(JZListWidgetItem* item,
                                                       const jsbind::JSRawPointer<JZListWidgetItem>& src) const;

    void invalidateItemJs(JZListWidgetItem* item) const;
    void invalidateAllItemJs() const;
    // 剔除已不在列表中的项开关(借用的兜底失效; 已交回 JS 的只丢开关, 不失效)。
    void pruneItemJs() const;

    mutable std::vector<ItemSwitch> m_itemJs;
};

#endif // !JZ_JS_LIST_WIDGET_H_
