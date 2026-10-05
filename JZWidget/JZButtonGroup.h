#ifndef JZ_BUTTON_GROUP_H_
#define JZ_BUTTON_GROUP_H_
#include <vector>
#include "JZObject.h"
#include "JZObjectPointer.h"
#include "JZSignal.h"

class JZWidget;

// 按钮组 (等价 Qt QButtonGroup): 把若干**可选中控件**(JZCheckbox / JZRadioButton / JZSwitch)
// 编成一组, 在组内维护互斥 —— 任一按钮被选中时自动取消其余按钮的选中态。
//
// 与 Qt 一致:
//   * 组**不拥有**按钮: 生命周期仍归其父控件, 这里只登记 + 跟踪存活 (按钮先销毁自动失效);
//   * 互斥开关 setExclusive (默认 true); 互斥组内**用户点击无法取消当前选中项**
//     (程序化 setChecked(false) 仍可取消);
//   * 成员可用整数 id 标识 (addButton 不传 id 时自动分配**负数** id, 与 Qt 同款),
//     并提供 button(id) / id(button) / checkedId() 便于取"选了哪一项"。
//   * 组的选中态变化发 sigButtonToggled / sigIdToggled, 点击发 sigButtonClicked / sigIdClicked。
//
// 用法:
//   auto* g = new JZButtonGroup(this);
//   g->addButton(r1, 0); g->addButton(r2, 1); g->addButton(r3, 2);
//   connect(&g->sigIdClicked, [](int id) { /* 选了第 id 项 */ });
//
// 说明: 只有 isCheckable() 为真的控件能入组; JZPushButton 当前不可选中, 不入组。
// JZRadioButton 未入组时保留"同父级兄弟自动互斥"的老行为, 入组后由本类统一互斥。
class JZFRAMEWORK_API JZButtonGroup : public JZObject
{
    JZ_OBJECT_DECLARE(JZButtonGroup)

public:
    explicit JZButtonGroup(JZObject* parent = nullptr);
    ~JZButtonGroup() override;

    // ---- 成员管理 ----
    // 返回 false: 控件为空 / 不可选中 / 已在组内 (Qt: 重复添加被忽略)。id 传 -1 表示自动分配。
    bool addButton(JZWidget* button, int id = -1);
    void removeButton(JZWidget* button);
    bool contains(JZWidget* button) const;

    std::vector<JZWidget*> buttons() const;   // 组内仍存活的成员 (按加入顺序)
    JZWidget* button(int id) const;           // 该 id 的第一个成员; 无则 nullptr
    int id(JZWidget* button) const;           // 不在组内返回 -1
    void setId(JZWidget* button, int id);     // 改成员 id (已在组内才生效)

    // ---- 选中态 ----
    JZWidget* checkedButton() const;          // 组内当前选中项; 无则 nullptr
    int checkedId() const;                    // 无选中返回 -1

    // ---- 互斥 ----
    void setExclusive(bool exclusive);        // 默认 true
    bool isExclusive() const;

jz_signals:
    JZSignal<JZWidget*>       sigButtonClicked;   // (button)         成员被点击
    JZSignal<int>             sigIdClicked;       // (id)             成员被点击
    JZSignal<JZWidget*, bool> sigButtonToggled;   // (button, checked) 成员选中态变化
    JZSignal<int, bool>       sigIdToggled;       // (id, checked)     成员选中态变化

private:
    friend class JZWidget;   // 供 JZWidget::notifyButtonGroup* 回调

    struct Entry
    {
        JZObjectPointer button;   // 守卫: 成员先销毁自动失效
        int id = -1;
    };

    // 供 JZWidget 回调 (见 JZWidget::notifyButtonGroupChecked / notifyButtonGroupClicked)
    void onButtonChecked(JZWidget* button);
    void onButtonClicked(JZWidget* button);

    Entry* entryOf(JZWidget* button);
    void pruneDead();

    std::vector<Entry> m_entries;
    JZObjectPointer m_checkedButton;   // 守卫: 选中项先销毁自动失效
    bool m_exclusive = true;
    bool m_updating = false;           // 互斥联动重入保护 (组内 setChecked(false) 期间不再回调)
    int m_nextAutoId = -2;             // 自动 id: 从 -2 起递减 (与 Qt 一致, 避开用户常用正数)
};

#endif // JZ_BUTTON_GROUP_H_
