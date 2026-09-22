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
declare const JZRect: any;
declare const jzApp: any;

// 本包的程序里没有共享 types(types/index.d.ts 只 reference 自己的文件), 所以按
// events/index.ts 的同样约定就地声明引擎注入的 console(只用到 error)。
declare var console: { error(...a: any[]): void };

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

    // -----------------------------------------------------------------------
    // exec(): Qt 风格的**同步**弹层 —— `const act = menu.exec(w.mapToGlobal(pt))`
    //
    // 为什么能同步: C++ 的 popup() 内部本来就是阻塞模态循环(execModal, 关闭才返回), 只是它
    // 没有返回值。"点了哪一项"由菜单的 sigActionTriggered 告知 —— 这里在弹层期间把它截下来,
    // popup() 返回时就能给出结果, **不需要**引擎再加接口。
    //
    // 三个要点:
    //   * 结果一律经本文件的缓存句柄(actionAt)取 ⇒ 与 addAction 拿到的是**同一份包装**,
    //     Qt 风格的 `act === act1` 才成立(引擎每次 toJsValueFromPtr 会造新包装并作废旧的)。
    //   * 子菜单里的项报的是**子菜单**的 onActionTriggered ⇒ 递归把子菜单也临时挂上。
    //   * 原来已有 onActionTriggered 的照旧调用(链式, 不吞别人的回调)。
    //
    // 实参形态: exec(anchor) / exec(owner, anchor) / exec(x, y)
    //   anchor = JZPoint(屏幕坐标, 最常用)或 JZRect; 省略 owner 时用 jzApp.activeWindow()
    //   (owner 只影响弹层的 DPI 缩放与定位基准, 没有也能弹 —— C++ 侧允许 owner 为 null)。
    // 返回: 被触发的菜单项(JZAction); 取消 / 点空白 -> null(与 Qt 一致)。
    //
    // 注: 引擎没绑 exec(它是本便利层新增的方法), 所以不能用上面的 patch()(那要求原方法已存在)。
    // -----------------------------------------------------------------------
    if (typeof JZMenu.prototype.exec !== 'function')
    {
        JZMenu.prototype.exec = function (this: any, ...args: any[]): any
        {
            const isWindowLike = (v: any) => !!v && typeof v.show === 'function' && typeof v.hide === 'function';
            const isPointLike = (v: any) => !!v && typeof v.x === 'function'
                && typeof v.y === 'function' && typeof v.width !== 'function';
            const isRectLike = (v: any) => !!v && typeof v.x === 'function' && typeof v.width === 'function'
                && typeof v.height === 'function' && !isWindowLike(v);

            let owner: any = null;
            let anchor: any = null;
            if (typeof args[0] === 'number' && typeof args[1] === 'number')
            {
                anchor = new JZRect(args[0], args[1], 1, 1);          // exec(x, y)
            }
            else if (isWindowLike(args[0]))
            {
                owner = args[0];                                      // exec(owner, anchor)
                anchor = args[1];
            }
            else
            {
                anchor = args[0];                                     // exec(anchor)
                if (isWindowLike(args[1]))
                    owner = args[1];
            }

            if (isPointLike(anchor))                                  // JZPoint -> 1×1 锚点矩形
                anchor = new JZRect(anchor.x(), anchor.y(), 1, 1);
            if (!anchor || !isRectLike(anchor))
            {
                console.error('[JZMenu.exec] 锚点需要 JZPoint / JZRect(或 x, y 两个数字)');
                return null;
            }

            const app: any = (typeof jzApp !== 'undefined') ? jzApp : null;
            if (!owner && app && typeof app.activeWindow === 'function')
                owner = app.activeWindow() || null;

            // 弹层期间截获"哪一项被触发"(子菜单也要挂: 它的项发的是它自己的信号)
            let hit: any = null;
            const saved: any[] = [];
            const hook = (m: any) =>
            {
                if (!m || typeof m.actionAt !== 'function')
                    return;
                const prev = m.onActionTriggered;
                saved.push({ menu: m, cb: prev });
                m.onActionTriggered = function (i: number)
                {
                    hit = { menu: m, index: i };
                    if (typeof prev === 'function')
                        prev(i);
                };
                const n = (typeof m.menuCount === 'function') ? m.menuCount() : 0;
                for (let k = 0; k < n; k++)
                    hook(m.menuAt(k));
            };
            hook(this);

            try
            {
                this.popup(owner, anchor);      // 阻塞到菜单关闭(内部 execModal)
            }
            finally
            {
                for (let i = 0; i < saved.length; i++)
                    saved[i].menu.onActionTriggered = saved[i].cb;
            }

            if (!hit)
                return null;                    // 取消 / 点空白: 与 Qt 一样返回空
            return hit.menu.actionAt(hit.index) || null;
        };
    }
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
