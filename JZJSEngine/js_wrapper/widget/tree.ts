// @jz/widget —— 真控件的**对象树**支持(父子登记 / 保活 / 弱引用父)。
//
// 为什么要有这一层:
//   引擎(2026-09-17 起的新模型)**不再做归属权转移**: JS 包装归 GC, 包装被回收 ⇒ 底层 C++ 对象被
//   delete。于是"把控件挂进对象树"必须自己补两件事, 否则子控件的包装一被 GC, 底层控件就被删掉,
//   而它还挂在父控件的子树里 ⇒ 父控件引用到已 delete 的对象(悬垂):
//     * 保活: 宿主(父控件 / 布局)持一份子包装的**强引用**, 子控件在树上期间包装不会被回收;
//     * 弱引用父: 子只存 WeakRef(父), 不存强引用 —— 否则"父→子→父"成环, 只要还留着任意一个子控件,
//       整棵树(含顶层窗口)就永远回收不掉。
//   quickjs 支持 WeakRef / FinalizationRegistry(ES2021 内建, 引擎用 JS_NewContext() 建的上下文自带)。
//
// 用法: `import '@jz/widget'`(或本包 index) —— 本模块加载即把下面这些方法装到全局类的原型上:
//   JZWidget   setParent / parent / parentWidget / children / setLayout / layout / clearLayout
//   JZMainWindow setCentralWidget / addMenuBar
//   JZWindow   addWidget
//   JZLayout   children / parentWidget
//   各具体布局  addWidget / insertWidget / addWidgetSpan / removeWidget / clear
//   视图容器    JZStackedWidget.addWidget / JZTabWidget.addTab / JZSplitter.addWidget|insertWidget
//               |removeWidget / JZScrollArea.setWidget
// 装完之后的语义(C++ 侧尚未注册这些方法, 全由本模块提供):
//   win.setParent(null)   脱离父树(不再被保活)
//   btn.parent()          拿回"你当初传进去的那份"父包装(=== 成立, 前提: 关系是 JS 侧建的)
//   win.children()        JS 侧登记过的直接子控件
//   lay.children()        加进这个布局的控件
//   lay.parentWidget()    布局的宿主控件(setLayout 过才有)
//
// ⚠ 覆盖面: 只有**经过 JS 侧这些入口**建立的关系才登记得下来。构造时直接带父控件
//   (`new JZLabel(win)`)不经过本层, 登记不到 —— 那种写法请补一句 `label.setParent(win)`,
//   否则 `win.children()` 里没有它、`label.parent()` 走原生兜底(现造包装, `===` 比不得)。
//   要彻底解决得在 C++ 侧做"同一底层对象 ⇒ 同一包装"的身份表(未做)。
//
// ⚠ 顶层窗口仍需自己留一份引用: 没人引用它 ⇒ 被 GC ⇒ 窗口 delete ⇒ 运行时退出。

// ---------------------------------------------------------------- 登记节点
// 节点挂在 WeakMap 上(键 = JS 包装), 而不是包装对象的属性上 —— 不污染包装, 也不怕被枚举到。
interface WidgetNode {
    /** 保活宿主(弱): 父控件的包装 / 布局的包装。宿主没了这份登记自然失效 */
    host?: WeakRef<JZWidget | JZLayout>;
    /** 父控件(弱): 经布局挂载时指向布局的宿主控件 */
    parent?: WeakRef<JZWidget>;
    /** 子控件(强) —— 这就是"保活" */
    children: JZWidget[];
    /** 布局(强): 布局归控件所有, 它的包装不能被 GC 掉 */
    layout?: JZLayout;
}

interface LayoutNode {
    /** 宿主控件(弱) */
    host?: WeakRef<JZWidget>;
    /** 布局里的控件(强) —— 保活 + children() */
    children: JZWidget[];
}

const widgetNodes = new WeakMap<JZWidget, WidgetNode>();
const layoutNodes = new WeakMap<JZLayout, LayoutNode>();

function widgetNode(w: JZWidget, create: boolean): WidgetNode | undefined {
    let n = widgetNodes.get(w);
    if (!n && create) {
        n = { children: [] };
        widgetNodes.set(w, n);
    }
    return n;
}

function layoutNode(l: JZLayout, create: boolean): LayoutNode | undefined {
    let n = layoutNodes.get(l);
    if (!n && create) {
        n = { children: [] };
        layoutNodes.set(l, n);
    }
    return n;
}

function hostOf(layout: JZLayout): JZWidget | undefined {
    const n = layoutNodes.get(layout);
    return n && n.host ? n.host.deref() : undefined;
}

function childListOf(host: JZWidget | JZLayout, create: boolean): JZWidget[] | undefined {
    if (host instanceof JZLayout) {
        const n = layoutNode(host, create);
        return n ? n.children : undefined;
    }
    const n = widgetNode(host, create);
    return n ? n.children : undefined;
}

function pushChild(host: JZWidget | JZLayout, child: JZWidget): void {
    const list = childListOf(host, true);
    if (list && list.indexOf(child) < 0) list.push(child);
}

function dropChild(host: JZWidget | JZLayout, child: JZWidget): void {
    const list = childListOf(host, false);
    if (!list) return;
    const i = list.indexOf(child);
    if (i >= 0) list.splice(i, 1);
}

/** 按序号插入(与 C++ 侧 insertWidget(i, w) 的存放顺序保持一致 -> widget(i) 才对得上) */
function insertChildAt(host: JZWidget, index: number, child: JZWidget): void {
    unlink(child);
    const list = childListOf(host, true);
    if (!list) return;
    const at = Math.max(0, Math.min(index, list.length));
    list.splice(at, 0, child);

    const n = widgetNode(child, true)!;
    n.host = new WeakRef(host);
    n.parent = new WeakRef(host);
}

/** 按序号摘除(C++ 侧 removeWidget(i) / removeTab(i) 是按下标删的) */
function removeChildAt(host: JZWidget, index: number): void {
    const list = childListOf(host, false);
    if (!list) return;
    const child = list[index];
    if (!child) return;
    list.splice(index, 1);
    const n = widgetNode(child, false);
    if (n) {
        n.host = undefined;
        n.parent = undefined;
    }
}

/** 把 child 挂到 host 下(保活)并记下父控件(弱)。host 是布局时父控件取布局的宿主控件 */
function link(host: JZWidget | JZLayout, child: JZWidget): void {
    unlink(child);                       // 换父: 先从旧宿主上摘掉
    pushChild(host, child);

    const parent = host instanceof JZLayout ? hostOf(host) : host;
    const n = widgetNode(child, true)!;
    n.host = new WeakRef(host);
    n.parent = parent ? new WeakRef(parent) : undefined;
}

/** 脱离: 从宿主的子表里摘掉 + 清掉弱引用(之后不再被任何宿主保活) */
function unlink(child: JZWidget): void {
    const n = widgetNode(child, false);
    if (!n) return;

    const host = n.host ? n.host.deref() : undefined;
    if (host) dropChild(host, child);
    n.host = undefined;
    n.parent = undefined;
}

function parentOf(child: JZWidget): JZWidget | null {
    const n = widgetNode(child, false);
    const p = n && n.parent ? n.parent.deref() : undefined;
    return p === undefined ? null : p;
}

/** 布局挂到控件下: 控件强引用布局 + 补上"先 addWidget 后 setLayout"那些子控件的父指向 */
function bindLayoutTo(host: JZWidget, layout: JZLayout): void {
    const hn = widgetNode(host, true)!;
    hn.layout = layout;

    const ln = layoutNode(layout, true)!;
    ln.host = new WeakRef(host);
    for (const c of ln.children) {
        const cn = widgetNode(c, true)!;
        cn.parent = new WeakRef(host);
        if (hn.children.indexOf(c) < 0) hn.children.push(c);
    }
}

/** clearLayout: 布局对象被删, 断开它对子控件的保活(子控件仍由宿主控件保活) */
function unbindLayout(host: JZWidget): void {
    const hn = widgetNode(host, false);
    if (!hn || !hn.layout) return;
    const ln = layoutNode(hn.layout, false);
    if (ln) ln.children = [];
    hn.layout = undefined;
}

// ---------------------------------------------------------------- 安装
let installed = false;

/**
 * 把对象树方法装到全局类的原型上(幂等)。模块加载时会自动调一次;
 * 引擎没注册控件类(没开 widget 模块)时静默跳过。
 */
export function installWidgetTree(): void {
    if (installed) return;
    if (typeof JZWidget === 'undefined') return;
    installed = true;

    // ---- JZWidget ----
    const nativeSetParent = JZWidget.prototype.setParentWidget;
    const nativeParent = JZWidget.prototype.parentWidget;
    const nativeSetLayout = JZWidget.prototype.setLayout;
    const nativeLayout = JZWidget.prototype.layout;
    const nativeClearLayout = JZWidget.prototype.clearLayout;

    JZWidget.prototype.setParent = function (this: JZWidget, parent: JZWidget | null): void {
        nativeSetParent.call(this, parent);
        if (parent) link(parent, this);
        else unlink(this);
    };

    JZWidget.prototype.setParentWidget = function (this: JZWidget, parent: JZWidget | null): void {
        this.setParent(parent);
    };

    // 登记过的直接返回"你当初传进去的那份"包装; 没登记过(构造时带父)走原生兜底 ——
    // 那份是现造的包装, 与你的变量 `===` 不成立(见文件头 ⚠ 覆盖面)。
    JZWidget.prototype.parent = function (this: JZWidget): JZWidget | null {
        const p = parentOf(this);
        if (p)
            return p;
        // 兜底: 只有关系不是经 JS 侧入口建立时才走原生(且该方法可能压根没注册 ——
        // 它返回指针, 涉及"同一底层对象 ⇒ 同一包装"的问题, 目前由本模块自己维护)
        if (typeof nativeParent === 'function')
            return nativeParent.call(this);
        return null;
    };
    JZWidget.prototype.parentWidget = JZWidget.prototype.parent;

    JZWidget.prototype.children = function (this: JZWidget): JZWidget[] {
        const n = widgetNode(this, false);
        return n ? n.children.slice() : [];
    };

    JZWidget.prototype.setLayout = function (this: JZWidget, layout: JZLayout): void {
        // C++ 的 setLayout 是 Qt 语义(已有布局 / null → 警告 + 忽略, 不抛), 并返回"是否被接受"。
        // JS 侧的约定见 types/widgets.d.ts: 这两种情况**抛 TypeError**(一个控件只能设一次布局,
        // 换布局先 clearLayout()), 而且只有被接受了才登记 —— 否则 w.layout() 会返回被拒的那份。
        // 注: 声明里 setLayout 是 void, 这里按内部约定取返回值(所以先转 any)。
        const accepted: any = (nativeSetLayout as any).call(this, layout);
        if (accepted === false)
            throw new TypeError('setLayout: 一个控件只能有一个布局, 且只能设一次(换布局请先 clearLayout(); null 不是清空手段)');
        if (layout)
            bindLayoutTo(this, layout);
    };

    JZWidget.prototype.layout = function (this: JZWidget): JZLayout | null {
        // 先看 JS 侧登记(本模块维护的"同一布局 ⇒ 同一包装"); 没登记过才走原生兜底。
        // ⚠ nativeLayout 可能压根没注册(它返回指针, 涉及"同一底层对象 ⇒ 同一包装"), 别直接 .call。
        const n = widgetNode(this, false);
        if (n && n.layout)
            return n.layout;
        if (typeof nativeLayout === 'function') {
            const l = nativeLayout.call(this);
            if (l) (widgetNode(this, true) as WidgetNode).layout = l;
            return l;
        }
        return null;
    };

    JZWidget.prototype.clearLayout = function (this: JZWidget): void {
        unbindLayout(this);
        nativeClearLayout.call(this);
    };

    // ---- JZGroupBox: 布局实际装到内容区(C++ 侧 JZGroupBox::setLayout 转发), JS 侧登记挂自己身上 ----
    // 注: 内容区那个控件是 C++ 内部造的, JS 侧压根没有它的对象(也不打算中转造一份) —— 所以这里的
    // 宿主用 groupbox 自己(保活 / layout.parentWidget() 都指它)。子控件从布局侧拿(布局是 JS 对象 ✅)。
    if (typeof JZGroupBox !== 'undefined') {
        const nativeBoxSetLayout = JZGroupBox.prototype.setLayout;
        const nativeBoxClearLayout = JZGroupBox.prototype.clearLayout;
        JZGroupBox.prototype.setLayout = function (this: JZGroupBox, layout: JZLayout): void {
            const accepted: any = (nativeBoxSetLayout as any).call(this, layout);   // 虚: C++ 转发到内容区
            if (accepted === false)
                throw new TypeError('setLayout: 一个控件只能有一个布局, 且只能设一次(换布局请先 clearLayout()); null 不是清空手段');
            if (layout)
                bindLayoutTo(this, layout);
        };
        JZGroupBox.prototype.clearLayout = function (this: JZGroupBox): void {
            unbindLayout(this);
            nativeBoxClearLayout.call(this);
        };
    }

    // ---- JZWindow: 加进窗口自带垂直布局 ----
    if (typeof JZWindow !== 'undefined') {
        const nativeWindowAdd = JZWindow.prototype.addWidget;
        JZWindow.prototype.addWidget = function (this: JZWindow, child: JZWidget): void {
            nativeWindowAdd.call(this, child);
            link(this, child);
        };
    }

    // ---- JZMainWindow: 中央控件 / 菜单栏 ----
    if (typeof JZMainWindow !== 'undefined') {
        const nativeCentral = JZMainWindow.prototype.setCentralWidget;
        JZMainWindow.prototype.setCentralWidget = function (this: JZMainWindow, widget: JZWidget): void {
            nativeCentral.call(this, widget);
            link(this, widget);
        };
        const nativeMenuBar = JZMainWindow.prototype.addMenuBar;
        JZMainWindow.prototype.addMenuBar = function (this: JZMainWindow, menuBar: JZMenuBar): void {
            nativeMenuBar.call(this, menuBar);
            link(this, menuBar);
        };
    }

    // ---- 布局: 宿主控件 / 子控件 ----
    JZLayout.prototype.children = function (this: JZLayout): JZWidget[] {
        const n = layoutNode(this, false);
        return n ? n.children.slice() : [];
    };
    JZLayout.prototype.parentWidget = function (this: JZLayout): JZWidget | null {
        const h = hostOf(this);
        return h === undefined ? null : h;
    };

    // 盒子布局(VBox / HBox): addWidget(w, stretch) / insertWidget(i, w, stretch)
    const patchBoxLayout = (proto: JZVBoxLayout | JZHBoxLayout): void => {
        const p = proto as any;
        const nativeAdd = p.addWidget;
        p.addWidget = function (this: JZLayout, w: JZWidget, stretch: number): void {
            nativeAdd.call(this, w, stretch);
            link(this, w);
        };
        const nativeInsert = p.insertWidget;
        p.insertWidget = function (this: JZLayout, index: number, w: JZWidget, stretch: number): void {
            nativeInsert.call(this, index, w, stretch);
            link(this, w);
        };
    };
    if (typeof JZVBoxLayout !== 'undefined') patchBoxLayout(JZVBoxLayout.prototype);
    if (typeof JZHBoxLayout !== 'undefined') patchBoxLayout(JZHBoxLayout.prototype);

    // 网格布局: addWidget(w, row, col) / addWidgetSpan(w, r, c, rs, cs)
    if (typeof JZGridLayout !== 'undefined') {
        const p = JZGridLayout.prototype as any;
        const nativeAdd = p.addWidget;
        p.addWidget = function (this: JZGridLayout, w: JZWidget, row: number, col: number): void {
            nativeAdd.call(this, w, row, col);
            link(this, w);
        };
        const nativeSpan = p.addWidgetSpan;
        p.addWidgetSpan = function (this: JZGridLayout, w: JZWidget, r: number, c: number,
            rs: number, cs: number): void {
            nativeSpan.call(this, w, r, c, rs, cs);
            link(this, w);
        };
    }

    // 自布局: addWidget(w, anchor, dx, dy)
    if (typeof JZSelfLayout !== 'undefined') {
        const p = JZSelfLayout.prototype as any;
        const nativeAdd = p.addWidget;
        p.addWidget = function (this: JZSelfLayout, w: JZWidget, anchor: number, dx: number, dy: number): void {
            nativeAdd.call(this, w, anchor, dx, dy);
            link(this, w);
        };
    }

    // 摘除 / 清空: removeWidget 精确解除; clear 放开全部保活
    // (removeAt 不处理 —— 布局 item 里还有 spacing/stretch, 下标对不上控件, 宁可多保活)
    const patchLayoutRemove = (proto: any): void => {
        if (!proto) return;
        const nativeRemove = proto.removeWidget;
        proto.removeWidget = function (this: JZLayout, w: JZWidget): void {
            if (nativeRemove) nativeRemove.call(this, w);
            unlink(w);
        };
        const nativeClear = proto.clear;
        proto.clear = function (this: JZLayout): void {
            if (nativeClear) nativeClear.call(this);
            const n = layoutNode(this, false);
            if (n) n.children = [];
        };
    };
    patchLayoutRemove(typeof JZVBoxLayout !== 'undefined' ? (JZVBoxLayout.prototype as any) : undefined);
    patchLayoutRemove(typeof JZHBoxLayout !== 'undefined' ? (JZHBoxLayout.prototype as any) : undefined);
    patchLayoutRemove(typeof JZGridLayout !== 'undefined' ? (JZGridLayout.prototype as any) : undefined);
    patchLayoutRemove(typeof JZSelfLayout !== 'undefined' ? (JZSelfLayout.prototype as any) : undefined);

    // ---- 视图容器: 登记子控件 + 按序号取回 ----
    // `widget(i)` / `currentWidget()` 这类"回吐控件"的 getter **只能在 JS 侧回答**:
    // C++ 侧只有裸指针, 变不回同一个 JS 对象(引擎没有身份表)。这里按 add/insert/remove 的序号
    // 维护登记表(与 C++ 的存放顺序一致), 于是下标能对上, 拿回的也是你当初传进去的那份。
    if (typeof JZStackedWidget !== 'undefined') {
        const ps = JZStackedWidget.prototype as any;
        const nativeAdd = ps.addWidget;
        const nativeInsert = ps.insertWidget;
        const nativeRemoveIndex = ps.removeWidget;
        ps.addWidget = function (this: JZStackedWidget, page: JZWidget): number {
            const i = nativeAdd.call(this, page);
            link(this, page);
            return i;
        };
        ps.insertWidget = function (this: JZStackedWidget, index: number, page: JZWidget): void {
            nativeInsert.call(this, index, page);
            insertChildAt(this, index, page);
        };
        ps.removeWidget = function (this: JZStackedWidget, index: number): void {
            nativeRemoveIndex.call(this, index);
            removeChildAt(this, index);
        };
        ps.widget = function (this: JZStackedWidget, index: number): JZWidget | null {
            const n = widgetNode(this, false);
            return n && n.children[index] ? n.children[index] : null;
        };
        ps.currentWidget = function (this: JZStackedWidget): JZWidget | null {
            const i = this.currentIndex();
            return i >= 0 ? this.widget(i) : null;
        };
    }
    if (typeof JZTabWidget !== 'undefined') {
        const pt = JZTabWidget.prototype as any;
        const nativeAdd = pt.addTab;
        const nativeRemoveIndex = pt.removeTab;
        pt.addTab = function (this: JZTabWidget, page: JZWidget, label: string): number {
            const i = nativeAdd.call(this, page, label);
            link(this, page);
            return i;
        };
        pt.removeTab = function (this: JZTabWidget, index: number): void {
            nativeRemoveIndex.call(this, index);
            removeChildAt(this, index);
        };
        pt.widget = function (this: JZTabWidget, index: number): JZWidget | null {
            const n = widgetNode(this, false);
            return n && n.children[index] ? n.children[index] : null;
        };
        pt.currentWidget = function (this: JZTabWidget): JZWidget | null {
            const i = this.currentIndex();
            return i >= 0 ? this.widget(i) : null;
        };
    }
    if (typeof JZSplitter !== 'undefined') {
        const ps = JZSplitter.prototype as any;
        const nativeAdd = ps.addWidget;
        const nativeInsert = ps.insertWidget;
        const nativeRemove = ps.removeWidget;
        ps.addWidget = function (this: JZSplitter, w: JZWidget): void {
            nativeAdd.call(this, w);
            link(this, w);
        };
        ps.insertWidget = function (this: JZSplitter, index: number, w: JZWidget): void {
            nativeInsert.call(this, index, w);
            insertChildAt(this, index, w);
        };
        ps.removeWidget = function (this: JZSplitter, w: JZWidget): void {
            nativeRemove.call(this, w);
            unlink(w);
        };
        ps.widget = function (this: JZSplitter, index: number): JZWidget | null {
            const n = widgetNode(this, false);
            return n && n.children[index] ? n.children[index] : null;
        };
    }
    if (typeof JZScrollArea !== 'undefined') {
        const nativeSetWidget = JZScrollArea.prototype.setWidget;
        JZScrollArea.prototype.setWidget = function (this: JZScrollArea, content: JZWidget): void {
            nativeSetWidget.call(this, content);
            link(this, content);
        };
        // 内容控件(setWidget 设进来的)是 JS 对象 -> 登记表里就有它, 拿回同一份
        JZScrollArea.prototype.widget = function (this: JZScrollArea): JZWidget | null {
            const n = widgetNode(this, false);
            return n && n.children.length ? n.children[n.children.length - 1] : null;
        };
    }

    // ---- 构造时直接带父的补登记 ----
    // `new JZLabel('x', win)` / `new JZPushButton('ok', win)` 这类写法: C++ 侧构造器自己就把父子
    // 关系建好了(不经过上面那些包装函数), 本模块不知情 —— 而**保活**和 parent()/children() 全靠
    // 这张登记表。所以引擎在构造完成后回调这个钩子, 由这里补一条登记(与 setParent 同一条路径)。
    (globalThis as any).__jzWidgetLink = function (parent: JZWidget, child: JZWidget): void {
        link(parent, child);
    };
}

installWidgetTree();
