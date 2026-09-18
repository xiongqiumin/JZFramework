// @jz/widget —— 列表 / 表格的**视图镜像**(dataview): JS 侧一张表 + 与控件 1:1 绑定, 双向同步。
//
// 为什么有这一层:
//   控件(C++ / owner-draw 虚拟绘制)只认"整行文本", 而且**不向 JS 暴露任何 item 句柄**
//   (见 types/views.d.ts 的说明)。写界面的人关心的是一张表: 加一行 / 删一行 / 改一行,
//   然后控件上就跟着变。视图镜像把这两件事接起来, 而且**主要工作都在 JS 侧**(C++ 只提供
//   "整块喂 / 整块读 / 单行增删"的朴素原语, 见 JZJSWidgetModule::registViews 的 dataview 段)。
//
//     const dv = table.dataView();     // 取视图(同一控件只有一份, 复用)
//     dv.push(["A", 1]);               // 视图加行 -> 控件立即多一行
//     dv[0] = ["B", 2];                // 下标赋值 = 换整行
//     dv.splice(1, 1);                 // 删一行(数组习惯)
//     dv.rows();                       // 快照(普通 JS 值)
//
//   反向: 控件侧的数据变了(别的 C++ 代码 / AI 工具的 invoke / 将来内联编辑)会发
//   `onDataChanged`, 视图收到后**自动重读**镜像 —— 所以 `dv[i]` 永远是最新的。
//   视图自己发起的那次变更会被静默(否则"视图改控件、控件又通知视图"会成环)。
//
// 三条约定:
//   1. 视图是**值级**的: 读出来的是副本(改副本不会同步, 也不会脏镜像)。要改就用方法或下标赋值。
//   2. 单元格只收 字符串 / 数字 / 布尔(统一按文本存进 C++); 其它类型抛 TypeError —— 免得
//      "[object Object]" 这类脏数据被悄悄写进去。
//   3. 一个控件一份视图(1:1, 不需要多源); `dv.dispose()` 解绑, 并把 onDataChanged 还给原来那个。

type CellText = string | number | boolean;

// 值拷贝: 表格的行是数组(拷一层), 列表的项是字符串(原样)
function copyValue(v: any): any
{
    return Array.isArray(v) ? (v as any[]).slice() : v;
}

// "123" 这种下标键(用于代理的 get/set 拦截)
function isIndexKey(p: string | symbol): boolean
{
    if (typeof p !== 'string' || p.length === 0)
        return false;
    for (let i = 0; i < p.length; i++)
    {
        const c = p.charCodeAt(i);
        if (c < 48 || c > 57)
            return false;
    }
    return true;
}

function cellText(v: any, what: string): string
{
    const t = typeof v;
    if (t === 'string')
        return v as string;
    if (t === 'number' || t === 'boolean')
        return String(v);
    throw new TypeError(what + ': 单元格只收 字符串/数字/布尔');
}

function rowCells(v: any, what: string): string[]
{
    if (!Array.isArray(v))
        throw new TypeError(what + ': 一行需要是数组, 如 ["A", 1]');
    return (v as any[]).map((c) => cellText(c, what));
}

// 已取的视图: 同一控件只有一份(视图与控件 1:1 绑定, 不做多源)
const views = new WeakMap<object, any>();

// ---------------------------------------------------------------- 基类

class DataViewBase
{
    protected w: any;
    protected mirror: any[] = [];      // JS 侧镜像(视图读写在它上面, 与控件保持同步)
    protected applying = false;        // 正在把视图变更写进控件(这期间的 onDataChanged 不回灌)
    protected what: string;
    protected owner: any = null;       // dispose 用(拿到代理那份)

    /** 用户回调槽: 用 `dv.onChanged = fn` 挂, 不要去改 `w.onDataChanged`(那会被视图接管) */
    onChanged: ((...args: any[]) => void) | null = null;

    protected userHook: any = null;    // 取视图前用户已经挂在 onDataChanged 上的那个

    constructor(w: any, what: string)
    {
        this.w = w;
        this.what = what;

        // 接管 onDataChanged: 先刷新镜像, 再转给"原来那个" handler(链式, 不吞掉)
        const user = w.onDataChanged;
        this.userHook = (typeof user === 'function') ? user : null;
        w.onDataChanged = () => {
            if (!this.applying)
                this.pull();
            if (this.userHook)
                this.userHook.call(w);
            if (this.onChanged)
                this.onChanged();
        };

        this.pull();
    }

    // ---- 子类实现: 读整块 / 三种写原语 ----
    protected read(): any[] { return []; }
    protected insertAt(_i: number, _value: any): void { }
    protected removeAt(_i: number): void { }
    protected replaceAt(_i: number, _value: any): void { }
    protected replaceAll(_values: any[]): void { }
    protected resize(_n: number): void { }

    protected pull(): void
    {
        this.mirror = this.read();
    }

    // 一次变更: 静默期 -> 写控件 -> 以控件为准重读(顺带带回 C++ 侧语义, 如"行比列宽自动加列")
    protected mutate(fn: () => void): void
    {
        this.applying = true;
        try
        {
            fn();
        }
        finally
        {
            this.applying = false;
        }
        this.pull();
    }

    // ---- 读 ----
    get length(): number { return this.mirror.length; }

    /** 一行的副本; 越界返回 null */
    row(i: number): any
    {
        const r = this.mirror[i];
        return r === undefined ? null : copyValue(r);
    }

    /** 整块快照(副本) */
    rows(): any[]
    {
        return this.mirror.map((r) => copyValue(r));
    }

    toArray(): any[] { return this.rows(); }

    /** 手动从控件重读(一般不用: 控件改了会自动同步) */
    refresh(): this
    {
        this.pull();
        return this;
    }

    forEach(fn: (row: any, index: number) => void): void
    {
        for (let i = 0; i < this.mirror.length; i++)
            fn(copyValue(this.mirror[i]), i);
    }

    map(fn: (row: any, index: number) => any): any[]
    {
        const out: any[] = [];
        for (let i = 0; i < this.mirror.length; i++)
            out.push(fn(copyValue(this.mirror[i]), i));
        return out;
    }

    *[Symbol.iterator](): IterableIterator<any>
    {
        for (let i = 0; i < this.mirror.length; i++)
            yield copyValue(this.mirror[i]);
    }

    // ---- 写(数组习惯) ----
    push(value: any): number
    {
        const at = this.mirror.length;
        this.mutate(() => this.insertAt(at, value));
        return this.mirror.length;
    }

    insert(i: number, value: any): void
    {
        const at = Math.max(0, Math.min(i, this.mirror.length));
        this.mutate(() => this.insertAt(at, value));
    }

    /** 删一行; 返回被删那份的副本(越界返回 null) */
    remove(i: number): any
    {
        if (i < 0 || i >= this.mirror.length)
            return null;
        const old = copyValue(this.mirror[i]);
        this.mutate(() => this.removeAt(i));
        return old;
    }

    removeRange(i: number, count: number): void
    {
        const at = Math.max(0, i);
        this.mutate(() => {
            for (let k = 0; k < count; k++)
                this.removeAt(at);
        });
    }

    set(i: number, value: any): void
    {
        if (i < 0 || i >= this.mirror.length)
            throw new RangeError(this.what + '.set: 行号越界 ' + i);
        this.mutate(() => this.replaceAt(i, value));
    }

    clear(): void
    {
        this.mutate(() => this.replaceAll([]));
    }

    /** 整块替换(与 setRows/setItems 等价) */
    setAll(values: any[]): void
    {
        this.mutate(() => this.replaceAll(values));
    }

    /** 改行数(多了补空行, 少了从尾部删) */
    setLength(n: number): void
    {
        if (n < 0)
            throw new RangeError(this.what + '.length: 不能为负');
        this.mutate(() => this.resize(n));
    }

    /** 数组习惯的批量删/插; 返回被删的那些(副本) */
    splice(start: number, deleteCount?: number, ...items: any[]): any[]
    {
        const len = this.mirror.length;
        const at = start < 0 ? Math.max(len + start, 0) : Math.min(start, len);
        const del = (deleteCount === undefined) ? (len - at)
                                                : Math.max(0, Math.min(deleteCount, len - at));
        const removed = this.mirror.slice(at, at + del).map((r) => copyValue(r));

        this.mutate(() => {
            for (let k = 0; k < del; k++)
                this.removeAt(at);                      // 连删同一位置 = 后面的依次前移
            for (let k = 0; k < items.length; k++)
                this.insertAt(at + k, items[k]);
        });
        return removed;
    }

    sort(cmp?: (a: any, b: any) => number): this
    {
        const sorted = this.mirror.slice().sort(cmp as any);
        this.mutate(() => this.replaceAll(sorted));
        return this;
    }

    reverse(): this
    {
        const rev = this.mirror.slice().reverse();
        this.mutate(() => this.replaceAll(rev));
        return this;
    }

    // ---- 生命周期 ----
    /** 解绑: 控件上不再自动回灌, onDataChanged 还给原来那个处理函数 */
    dispose(): void
    {
        if (this.owner && views.get(this.w) === this.owner)
            views.delete(this.w);
        this.w.onDataChanged = this.userHook ? this.userHook : null;
        this.userHook = null;
        this.onChanged = null;
    }

    /** 供 dispose/proxy 用: 记住代理那份 */
    __setOwner(o: any): void { this.owner = o; }
}

// ---------------------------------------------------------------- 表格(整行粒度)

class TableDataView extends DataViewBase
{
    protected cols: string[] = [];     // 表头标签(JS 侧记着, 设给控件只为显示)

    constructor(w: any)
    {
        super(w, 'JZTableWidget.dataView');
    }

    protected read(): any[]
    {
        return this.w.rows() as any[];
    }

    protected insertAt(i: number, value: any): void { this.w.insertRowAt(i, rowCells(value, this.what)); }
    protected removeAt(i: number): void { this.w.removeRowAt(i); }
    protected replaceAt(i: number, value: any): void { this.w.setRowAt(i, rowCells(value, this.what)); }
    protected replaceAll(values: any[]): void { this.w.setRows(values.map((r) => rowCells(r, this.what))); }
    protected resize(n: number): void { this.w.setRowCount(n); }

    /** 列(表头): 不传参数 = 读回标签; 传数组 = 设标签(同时记在视图上) */
    columns(labels?: CellText[]): any
    {
        if (labels === undefined)
            return this.cols.slice();

        this.cols = labels.map((s) => cellText(s, this.what + '.columns'));
        this.w.setHeaderLabels(this.cols);
        return this;
    }
}

// ---------------------------------------------------------------- 列表(单项粒度)

class ListDataView extends DataViewBase
{
    constructor(w: any)
    {
        super(w, 'JZListWidget.dataView');
    }

    protected read(): any[]
    {
        return this.w.items() as any[];
    }

    protected insertAt(i: number, value: any): void { this.w.insertItem(i, cellText(value, this.what)); }
    protected removeAt(i: number): void { this.w.removeItem(i); }
    protected replaceAt(i: number, value: any): void { this.w.setItemText(i, cellText(value, this.what)); }
    protected replaceAll(values: any[]): void { this.w.setItems(values.map((v) => cellText(v, this.what))); }

    protected resize(n: number): void
    {
        for (let i = this.length - 1; i >= n; i--)
            this.removeAt(i);
        for (let i = this.length; i < n; i++)
            this.insertAt(i, '');
    }
}

// ---------------------------------------------------------------- 数组下标代理

// 让 `dv[0]` / `dv[0] = row` / `for (const r of dv)` 都像数组: 下标读写落在视图的 row/set 上。
function wrapWithIndex(view: DataViewBase): any
{
    const proxy = new Proxy(view as any, {
        get(t, p, recv) {
            if (isIndexKey(p))
            {
                const r = t.row(Number(p));
                return r === null ? undefined : r;
            }
            return Reflect.get(t, p, recv);
        },
        set(t, p, v, recv) {
            if (isIndexKey(p))
            {
                t.set(Number(p), v);
                return true;
            }
            if (p === 'length')
            {
                t.setLength(Number(v));
                return true;
            }
            return Reflect.set(t, p, v, recv);
        },
        has(t, p) {
            if (isIndexKey(p))
                return Number(p) < t.length;
            return Reflect.has(t, p);
        },
        deleteProperty(_t, p) {
            throw new TypeError('视图不支持下标的 delete: 删行请用 remove(i) / splice(i, 1)');
        }
    });
    view.__setOwner(proxy);
    return proxy;
}

// ---------------------------------------------------------------- 入口

export function dataViewOf(w: any, kind: 'list' | 'table'): any
{
    const cached = views.get(w);
    if (cached)
        return cached;

    const view: DataViewBase = (kind === 'table') ? new TableDataView(w) : new ListDataView(w);
    const proxied = wrapWithIndex(view);
    views.set(w, proxied);
    return proxied;
}

// 挂到两个控件的原型上(与 tree.ts 同一套: 控件类由 classes.ts 定义, 这里只加方法)
if (typeof JZListWidget !== 'undefined')
{
    (JZListWidget.prototype as any).dataView = function (this: any) {
        return dataViewOf(this, 'list');
    };
}

if (typeof JZTableWidget !== 'undefined')
{
    (JZTableWidget.prototype as any).dataView = function (this: any) {
        return dataViewOf(this, 'table');
    };
}
