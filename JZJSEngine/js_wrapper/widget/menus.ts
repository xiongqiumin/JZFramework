// @jz/widget —— 菜单句柄(借用)的**保活 + 去重**层。
//
// 为什么需要这两件事:
//   JZMenuBar.addMenu / JZMenu.addAction(以及 menuAt/actionAt)交回的是**借用句柄** ——
//   底层 JZMenu/JZAction 由菜单栏/菜单持有(C++ 侧), JS 不负责 delete; 但按引擎的守卫约定,
//   句柄被 GC 时会"当自己是主人"把还活着的对象 delete 掉(见 JZJSBind.h 的 initOpaqueConvention)。
//   而 uiLoader 生成的 setupUi() 里这些句柄大多是**局部变量**, 一出作用域就可回收。
//
//   另外: C++ 侧"同一个对象"每次交给 JS 都是一份**新包装**(引擎不做 对象->包装 的身份表),
//   而宿主句柄(js_value)只有一份 —— 后发的包装会把前一份作废(前一份再用就报"对象已被销毁")。
//   所以这里按"位置"给同一个菜单/菜单项**只发一份**: 先问问缓存里有没有, 没有才问 C++ 要。
//
// 结果: `const a = m.addAction("退出"); ... m.actionAt(0) === a` 成立, 且句柄不会被回收/作废。

// 本文件只往原型上装包装, 没有需要导出的值(但被 index 的 `export *` 引, 得是个模块)
export {};

declare const JZMenu: any;
declare const JZMenuBar: any;

interface MenuHandles {
    actions: any[];    // 按下标缓存(可能留空位: 只缓存要过的那几个)
    menus: any[];      // 子菜单, 同上
}

const handles = new WeakMap<object, MenuHandles>();

function bag(owner: any): MenuHandles
{
    let b = handles.get(owner);
    if (!b)
    {
        b = { actions: [], menus: [] };
        handles.set(owner, b);
    }
    return b;
}

function cacheAt(list: any[], index: number, make: () => any): any
{
    if (index < 0)
        return make();

    let v = list[index];
    if (v === undefined || v === null)
    {
        v = make();
        if (v !== undefined && v !== null)
            list[index] = v;
    }
    return v === undefined ? null : v;
}

function patch(proto: any, name: string, impl: (self: any, orig: any, args: any[]) => any): void
{
    const orig = proto[name];
    if (typeof orig !== 'function' || orig.__jzMenuPatched)
        return;

    const wrapped = function (this: any, ...args: any[]): any {
        return impl(this, orig, args);
    };
    wrapped.__jzMenuPatched = true;
    proto[name] = wrapped;
}

if (typeof JZMenu !== 'undefined')
{
    const p = JZMenu.prototype;

    patch(p, 'addAction', (self, orig, args) => {
        const a = orig.apply(self, args);
        const b = bag(self);
        if (a !== null && a !== undefined)
            b.actions[b.actions.length] = a;   // 新项就在末尾(actionCount()-1)
        return a;
    });

    patch(p, 'actionAt', (self, orig, args) => {
        return cacheAt(bag(self).actions, args[0], () => orig.apply(self, args));
    });

    patch(p, 'addMenu', (self, orig, args) => {
        const m = orig.apply(self, args);
        const b = bag(self);
        if (m !== null && m !== undefined)
            b.menus[b.menus.length] = m;
        return m;
    });

    patch(p, 'menuAt', (self, orig, args) => {
        return cacheAt(bag(self).menus, args[0], () => orig.apply(self, args));
    });
}

if (typeof JZMenuBar !== 'undefined')
{
    const p = JZMenuBar.prototype;

    patch(p, 'addMenu', (self, orig, args) => {
        const m = orig.apply(self, args);
        const b = bag(self);
        if (m !== null && m !== undefined)
            b.menus[b.menus.length] = m;
        return m;
    });

    patch(p, 'menuAt', (self, orig, args) => {
        return cacheAt(bag(self).menus, args[0], () => orig.apply(self, args));
    });
}
