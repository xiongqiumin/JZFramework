// @jz/widget —— 控件的"通用属性"表(setProperty / property), **纯 JS 侧存储**(不落 C++)。
//
// 为什么放 JS 侧:
//   * 这些属性值本来就是 JS 值(数组 / 对象 / 函数 / 控件实例...) —— 存 C++ 要先包一层 metatype 句柄、
//     读回来再解一层; 放 JS 侧就是"同一份引用", `property('x') === 当初设进去的那个对象` 天然成立。
//   * C++ 侧没人读它们(引擎自己的"宿主句柄"走 JZObject::property("js_value"), 与这套无关),
//     也没有跨语言共享的需求 —— 绕一圈只会带来类型转换和生命周期问题。
//
//   w.setProperty(name, value)   // value 为 null / undefined = **移除**该属性
//   w.property(name)             // 没设过(或已移除) -> null
//
// 存储: 每个控件一份 Map, 用 WeakMap 按"控件那个 JS 对象"索引 —— 控件回收时属性跟着走,
// 而且**不产生额外保活**(不会因为挂了属性就让控件/值活得比本该的久)。

// 本文件只往原型上装方法, 没有需要导出的值; 但它是被 `export * from './props'` 引的模块,
// 所以显式标一下(否则 tsc 认为它不是模块)。
export {};

const bags = new WeakMap<object, Map<string, any>>();

function propBag(self: any): Map<string, any>
{
    let m = bags.get(self);
    if (!m)
    {
        m = new Map<string, any>();
        bags.set(self, m);
    }
    return m;
}

if (typeof JZWidget !== 'undefined')
{
    // 挂在 JZWidget 原型上 -> 所有控件(含子类)都有
    (JZWidget.prototype as any).setProperty = function (this: any, name: string, value: any): void {
        const m = propBag(this);
        const key = String(name);
        if (value === null || value === undefined)
            m.delete(key);
        else
            m.set(key, value);
    };

    (JZWidget.prototype as any).property = function (this: any, name: string): any {
        const m = bags.get(this);
        if (!m)
            return null;
        const v = m.get(String(name));
        return v === undefined ? null : v;
    };

    // 清空某控件的全部属性(内部用: 目前只给测试/调试)
    (JZWidget.prototype as any).__clearProperties = function (this: any): void {
        bags.delete(this);
    };
}
