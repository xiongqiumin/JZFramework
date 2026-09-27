#ifndef JZ_PROPERTY_ANIMATION_H_
#define JZ_PROPERTY_ANIMATION_H_
#include "JZAbstractAnimation.h"
#include <string>

// Qt QPropertyAnimation 语义的 int32 属性动画: 在 [start, end] 间插值,
// 每帧把当前值写入目标控件的同名属性。目标控件经 setTarget(JZWidget*) 指定。
//
// 支持的属性名 (不区分大小写, 均为 int32):
//   "x" / "y"            移动 (move)
//   "w" / "width"        宽度 (resize)
//   "h" / "height"       高度 (resize)
//   "geometry"           未支持: 几何是四维, 请拆成 x/y/w/h 四条动画
//
// 动画期间会令目标控件逐帧 invalidate, 无需手动 update。
class JZFRAMEWORK_API JZPropertyAnimation : public JZAbstractAnimation
{
    JZ_OBJECT_DECLARE(JZPropertyAnimation)

public:
    JZPropertyAnimation(JZObject* parent = nullptr);

    // from/to 显式给定; 返回 false = 属性名不支持或目标不是控件
    bool setProperty(const std::string& name, int32_t from, int32_t to);
    // 便捷版: from 取控件当前值 (从当前位置动画到 to)
    bool setProperty(const std::string& name, int32_t to);

    static bool isSupportedName(const std::string& name);
    std::string propertyName() const { return m_propName; }

protected:
    void onValueChanged(int32_t value) override;

private:
    int32_t currentValue() const;   // 目标控件当前属性值 (from 缺省用)
    bool applyToWidget(int32_t v);  // 把值写入目标控件属性

    std::string m_propName;         // 小写化后的属性名
};

#endif
