// 列表 / 表格的**视图镜像**(dataview) —— 运行期实现见 @jz/widget 的 dataview.ts
// (由本包 index 加载时挂到 JZTableWidget / JZListWidget 的原型上)。
//
//   const dv = table.dataView();     // 取视图(同一控件只有一份, 复用)
//   dv.push(["A", 1]);               // 视图加行 -> 控件立刻同步
//   dv[0] = ["B", 2];                // 下标赋值 = 换整行
//   dv.splice(1, 1);                 // 删一行(数组习惯)
//   dv.rows();                       // 快照(副本)
//
// 反向同步: 控件侧(别的 C++ 代码 / AI 工具 / 将来内联编辑)改了数据会发 onDataChanged,
// 视图收到后自动重读 —— 所以 `dv[i]` 始终是最新的; 视图自己发起的那次变更被静默(防成环)。
//
// 三条约定:
//   1. 值级: 读出来都是**副本**(改副本不会同步, 也不会脏镜像); 要改就用方法或下标赋值。
//   2. 单元格只收 字符串/数字/布尔(存进控件时统一按文本); 其它类型抛 TypeError。
//   3. 视图与控件 **1:1**(不需要多源): 一个控件一份视图, `dv.dispose()` 解绑。

/** 视图里的一格(表格的行是它的数组; 列表的项就是一个) */
type JZDataViewCell = string | number | boolean;

/** 表格的视图镜像(整行粒度) */
interface JZTableDataView extends Iterable<JZDataViewCell[]> {
  /** 行数(= 控件的 rowCount()); 赋值 = 改行数(多了补空行, 少了从尾部删) */
  length: number;

  /** 下标读 = 第 i 行的副本; 越界得到 undefined */
  [index: number]: JZDataViewCell[];

  // ---- 读 ----
  /** 一行的副本; 越界返回 null */
  row(i: number): JZDataViewCell[] | null;
  /** 整块快照(副本) */
  rows(): JZDataViewCell[][];
  toArray(): JZDataViewCell[][];
  forEach(fn: (row: JZDataViewCell[], index: number) => void): void;
  map<T>(fn: (row: JZDataViewCell[], index: number) => T): T[];
  /** 手动从控件重读(一般不用: 控件改了会自动同步) */
  refresh(): JZTableDataView;

  // ---- 写 ----
  /** 末尾加一行(行比当前列数多时会自动加列); 返回新行数 */
  push(row: JZDataViewCell[]): number;
  /** 在第 i 行前插入一行 */
  insert(i: number, row: JZDataViewCell[]): void;
  /** 删一行; 返回被删那份的副本(越界返回 null) */
  remove(i: number): JZDataViewCell[] | null;
  removeRange(i: number, count: number): void;
  /** 换整行(超出新行长度的多余格子会被清空); 越界抛 RangeError */
  set(i: number, row: JZDataViewCell[]): void;
  clear(): void;
  /** 整块替换(等价控件 setRows) */
  setAll(rows: JZDataViewCell[][]): void;
  setLength(n: number): void;
  /** 数组习惯的批量删/插; 返回被删的那些(副本) */
  splice(start: number, deleteCount?: number, ...rows: JZDataViewCell[][]): JZDataViewCell[][];
  sort(cmp?: (a: JZDataViewCell[], b: JZDataViewCell[]) => number): JZTableDataView;
  reverse(): JZTableDataView;

  // ---- 表头(列标题; 只是显示用, 数据仍是整行文本) ----
  columns(): string[];
  columns(labels: JZDataViewCell[]): JZTableDataView;

  // ---- 生命周期 / 回调 ----
  /** 解绑: 控件不再自动回灌, onDataChanged 还给原来那个处理函数 */
  dispose(): void;
  /** 视图刷新后回调(视图自己发起的那次也会走到); 想直接挂控件信号用控件.onDataChanged */
  onChanged: (() => void) | null;
}

/** 列表的视图镜像(单项粒度) */
interface JZListDataView extends Iterable<string> {
  /** 项数(= 控件的 count()); 赋值 = 改项数(少了从尾部删, 多了补空串) */
  length: number;

  /** 下标读 = 第 i 项的副本; 越界得到 undefined */
  [index: number]: string;

  row(i: number): string | null;
  rows(): string[];
  toArray(): string[];
  forEach(fn: (item: string, index: number) => void): void;
  map<T>(fn: (item: string, index: number) => T): T[];
  refresh(): JZListDataView;

  push(item: JZDataViewCell): number;
  insert(i: number, item: JZDataViewCell): void;
  remove(i: number): string | null;
  removeRange(i: number, count: number): void;
  set(i: number, item: JZDataViewCell): void;
  clear(): void;
  setAll(items: JZDataViewCell[]): void;
  setLength(n: number): void;
  splice(start: number, deleteCount?: number, ...items: JZDataViewCell[]): string[];
  sort(cmp?: (a: string, b: string) => number): JZListDataView;
  reverse(): JZListDataView;

  dispose(): void;
  onChanged: (() => void) | null;
}

// 控件上取视图(方法本体在 dataview.ts; 这里给类型用户做声明合并)
interface JZTableWidget {
  /** 视图镜像: 与控件 1:1 绑定(同一控件只有一份, 复用) */
  dataView(): JZTableDataView;
}

interface JZListWidget {
  /** 视图镜像: 与控件 1:1 绑定(同一控件只有一份, 复用) */
  dataView(): JZListDataView;
}
