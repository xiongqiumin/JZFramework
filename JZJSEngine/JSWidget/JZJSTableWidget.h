#ifndef JZ_JS_TABLE_WIDGET_H_
#define JZ_JS_TABLE_WIDGET_H_

#include "JZTableWidget.h"
#include "JZJSType.h"
#include "JZJSWidgetModule.h"   // JZSWidgetEventFilter(事件转发)

// JZTableWidget 的 JS 侧包装(镜像)类。命名约定与理由见 JZJSWidget.h 顶部说明
// (签名中出现 JS 指针类型的方法统一以 Js 结尾; JS 侧看到的名字由绑定层 def() 决定)。
//
// 与 JZJSListWidget 同一套路 —— 要解决的是**数据项的所有权/失效**:
//   JZTableWidgetItem 不是 JZObject(没有守卫指针, 对 delete 无感知), 只能用 JSRawPointer<T>,
//   失效必须有人显式 clear()。开关**按 item 指针索引**而不是 row/col: 插入/删除行会让行列
//   整体错位, 按行列存开关会把还活着的项误失效; item 指针在本表内稳定(见 JZTableWidget 的
//   unique_ptr 网格), 所以用它做键。改行列时用 reindex 维护的是 item 自己的 row()/column(),
//   与本开关表无关。
class JZJSTableWidget : public JZTableWidget
{
    JZ_OBJECT_DECLARE(JZJSTableWidget)

public:
    explicit JZJSTableWidget(JZWidget* parent = nullptr);
    ~JZJSTableWidget() override;

    // ---- 查询 (借用包装: cppOwner=true, 由表格持有) ----
    jsbind::JSRawPointer<JZTableWidgetItem> itemJs(int row, int col) const;   // 越界/空格返回空包装
    jsbind::JSRawPointer<JZTableWidgetItem> currentItemJs() const;

    // ---- 落位 / 接管 ----
    // 接管 JS 创建的单元格(/Transfer/, 对应 table.setItem(r, c, new Item()))
    // 注意与 Qt 一致: 该格原有的 item 会被删除 —— 本方法会先让旧 item 的 JS 包装失效。
    jsbind::JSRawPointer<JZTableWidgetItem> setItemJs(int row, int col, jsbind::JSRawPointer<JZTableWidgetItem>& item);
    // 便捷: 本表新建一个单元格(文本)并落位
    jsbind::JSRawPointer<JZTableWidgetItem> setItemJs(int row, int col, const std::string& text);

    // 传空包装 = 清空该格(旧 item 删除前先失效)
    void clearItemJs(int row, int col);

    // ---- 摘除 (必须走这里, 否则 JS 侧持有的包装会悬垂) ----
    void removeItemJs(int row, int col);                                     // 摘除并删除
    jsbind::JSRawPointer<JZTableWidgetItem> takeItemJs(int row, int col);    // 摘除并交回 JS 所有权(/TransferBack/)
    void removeRowJs(int row);                                               // 整行删除(先失效该行包装)
    void removeColumnJs(int col);                                            // 整列删除(先失效该列包装)
    void clearContentsJs();                                                  // 先失效再清空
    void clearJs();                                                          // 先失效再清空(含表头)

protected:
    struct ItemSwitch
    {
        JZTableWidgetItem* item = nullptr;
        jsbind::JSRawPointer<JZTableWidgetItem> sw;
    };

    // 按项指针取/建失效开关(新建的是"借用")。
    jsbind::JSRawPointer<JZTableWidgetItem>& switchFor(JZTableWidgetItem* item) const;
    // 用 src 的包装作为开关(共享同一 PointInfo), 使失效能传播回 JS 侧持有的那份。
    jsbind::JSRawPointer<JZTableWidgetItem> adoptSwitch(JZTableWidgetItem* item,
                                                        const jsbind::JSRawPointer<JZTableWidgetItem>& src) const;

    void invalidateItemJs(JZTableWidgetItem* item) const;
    void invalidateAllItemJs() const;
    // 剔除已不在表格中的项开关(借用的兜底失效; 已交回 JS 的只丢开关, 不失效)。
    void pruneItemJs() const;

    mutable std::vector<ItemSwitch> m_itemJs;
};

#endif // !JZ_JS_TABLE_WIDGET_H_
