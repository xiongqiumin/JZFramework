// 容器 / 视图类控件 —— 全局类(JZJSWidgetModule::registViews)。
//
// 这些控件的数据/子项由 C++ 持有; JS 侧只做**索引式值操作**(文本、行数、当前项、滚动位置),
// **不暴露任何 item 句柄**(没有 JZTableWidgetItem / JZListWidgetItem 这类东西) ——
// "值级原语 + 索引" 就是这层的全部接口形态: 整行/整项进出, 或者按下标读写单项/单格。
// 需要"数组式"批量处理时, 显式取快照再回写:
//     const rows = table.rows();  rows.splice(1, 1);  table.setRows(rows);
//
// 生命周期(与 JZWidget 同一套, 见 index.d.ts 的"生命周期 / 包活"): JS 包装归 GC, 挂进容器时
// 由 `@jz/widget` 运行期登记**保活**(容器持子控件的强引用, 子控件弱引用容器)。
// addWidget / insertWidget / addTab / setWidget 这些入口都已包好, 传进去的控件不需要自己 keep。

// 列表(选页 / 选择用)。与 Qt 一样**没有** removeItem 的"摘除"语义:
// 绑定层的 removeItem(i) 就是 takeItem(i) + delete, 确实会释放该项。
/** @final */
declare class JZListWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  // ---- 值级(直接操作控件) ----
  /** 末尾加一项 */
  appendItem(text: string): void;
  /** 批量追加 */
  appendItems(items: string[]): void;
  /** 在第 index 项前插入 */
  insertItem(index: number, text: string): void;
  /** 摘除并**释放**第 index 项 */
  removeItem(index: number): void;
  /** 清空 */
  clear(): void;
  count(): number;
  itemText(index: number): string;
  /** 改第 index 项的文本(越界忽略) */
  setItemText(index: number, text: string): void;
  /** 整表替换 */
  setItems(items: string[]): void;
  /** 整块读(文本数组) */
  items(): string[];
  currentIndex(): number;
  setCurrentIndex(index: number): void;
  setItemHeight(h: number): void;
  itemHeight(): number;

  /** 当前项索引变化(属性形) */
  onCurrentIndexChanged: ((index: number) => void) | null;
  /** 激活(点击项 / Enter)(属性形) */
  onActivated: ((index: number) => void) | null;
  /** 数据变化(增删项/改文本; 含 C++ 侧/AI 工具改的)(属性形, 单个处理器) */
  onDataChanged: (() => void) | null;
}

// 表格 —— **整行粒度**的数据视图(整行喂 / 整行读, 表格内部按文本格存)。
/** @final */
declare class JZTableWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  // ---- 行列 ----
  setRowCount(n: number): void;
  rowCount(): number;
  /** 在 row 处插一个空行(其后行号顺延) */
  insertRow(row: number): void;
  /** 删整行 */
  removeRow(row: number): void;
  setColumnCount(n: number): void;
  columnCount(): number;
  /** 在 col 处插一个空列 */
  insertColumn(col: number): void;
  /** 删整列 */
  removeColumn(col: number): void;

  // ---- 表头 ----
  /** 表头标签(只影响显示) */
  setHeaderLabels(labels: string[]): void;
  /** 读回表头标签 */
  headerLabels(): string[];

  // ---- 单元格(值级) ----
  /** 写时自动建单元格 */
  setItemText(row: number, col: number, text: string): void;
  itemText(row: number, col: number): string;
  /** 只清数据(item + 表头都清) */
  clearContents(): void;
  /** 清数据 + 表头标签 */
  clear(): void;

  // ---- 整行(值级) ----
  /** 末尾加一行(行比列数多时自动加列), 如 appendRow(["A", 1]) */
  appendRow(cells: (string | number | boolean)[]): void;
  insertRowAt(index: number, cells: (string | number | boolean)[]): void;
  /** 删一行(该行数据随之释放); 越界忽略 */
  removeRowAt(index: number): void;
  /** 换整行(超出的多余格子清空); 越界抛异常 */
  setRowAt(index: number, cells: (string | number | boolean)[]): void;
  /** 整表替换(列数按最宽的一行自动定) */
  setRows(rows: (string | number | boolean)[][]): void;
  /** 整表读(一律按文本返回) */
  rows(): string[][];

  // ---- 行高 / 列宽(看的) ----
  /** 全局行高(表头高度同此) */
  setRowHeight(h: number): void;
  rowHeight(): number;
  headerHeight(): number;
  /** 单列宽度 */
  setColumnWidth(col: number, w: number): void;

  // ---- 选择 ----
  setCurrentCell(row: number, col: number): void;
  currentRow(): number;
  currentColumn(): number;

  /** 当前行 / 列变化(属性形) */
  onCurrentRowChanged: ((row: number) => void) | null;
  onCurrentColumnChanged: ((col: number) => void) | null;
  /** 数据变化(行列增删/单元格改/表头; 含 C++ 侧/AI 工具改的)(属性形, 单个处理器) */
  onDataChanged: (() => void) | null;
}

// 页容器: 多个 page 叠放, 同一时刻只显示当前 page
/** @final */
declare class JZStackedWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 返回新页下标 */
  addWidget(page: JZWidget): number;
  /** 插到指定位置(下标与 widget(i) 一致) */
  insertWidget(index: number, page: JZWidget): void;
  /** 按**下标**摘除页面(**并删除该页面对象**) */
  removeWidget(index: number): void;
  count(): number;
  indexOf(page: JZWidget): number;
  /** 第 i 个页面; 关系是 JS 侧建的(addWidget/insertWidget)时拿回的就是你传进去的那份包装 */
  widget(index: number): JZWidget | null;
  currentIndex(): number;
  setCurrentIndex(index: number): void;
  currentWidget(): JZWidget | null;
}

// 标签页(页面所有权: 加入后由标签控件持有)
/** @final */
declare class JZTabWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 返回新页下标 */
  addTab(page: JZWidget, label: string): number;
  /** 按下标摘除页面(同时删除页面对象) */
  removeTab(index: number): void;
  count(): number;
  indexOf(page: JZWidget): number;
  /** 第 i 个页面(与 addTab 的顺序一致) */
  widget(index: number): JZWidget | null;
  currentWidget(): JZWidget | null;
  tabText(index: number): string;
  currentIndex(): number;
  setCurrentIndex(index: number): void;

  /** 当前页索引变化(属性形) */
  onCurrentChanged: ((index: number) => void) | null;
}

// 可拖分隔容器(方向: 0=Horizontal 横排 1=Vertical 纵排)
/** @final */
declare class JZSplitter extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);
  constructor(orientation: 0 | 1);
  constructor(orientation: 0 | 1, parent: JZWidget | null);

  setOrientation(orientation: 0 | 1): void;
  /** 0=Horizontal 1=Vertical */
  orientation(): number;
  addWidget(widget: JZWidget): void;
  insertWidget(index: number, widget: JZWidget): void;
  removeWidget(widget: JZWidget): void;
  count(): number;
  setStretchFactor(index: number, stretch: number): void;
  setHandleWidth(w: number): void;
  handleWidth(): number;
  setCollapsible(index: number, collapsible: boolean): void;
  /** (pos, handleIndex) 拖动中 / 结束都发(属性形) */
  onSplitterMoved: ((pos: number, handleIndex: number) => void) | null;
}

// 独立滚动条(方向: 0=Horizontal 1=Vertical)
/** @final */
declare class JZScrollBar extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);
  constructor(orientation: 0 | 1);
  constructor(orientation: 0 | 1, parent: JZWidget | null);

  setOrientation(orientation: 0 | 1): void;
  setRange(min: number, max: number): void;
  minimum(): number;
  maximum(): number;
  setValue(v: number): void;
  value(): number;
  setSingleStep(v: number): void;
  singleStep(): number;
  setPageStep(v: number): void;
  pageStep(): number;

  /** 值变化(属性形) */
  onValueChanged: ((value: number) => void) | null;
}

// 滚动区(setWidget 的内容控件由滚动区接管生命周期)
/** @final */
declare class JZScrollArea extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 内容控件挂到内部 viewport 下, 内容尺寸取自它的 width()/height() */
  setWidget(content: JZWidget): void;
  /** 取回 setWidget 设进去的那份包装(JS 侧登记的关系, `===` 成立) */
  widget(): JZWidget | null;
  /** 虚拟内容总尺寸(w/h < 0 表示该方向跟随视口) */
  setContentSize(w: number, h: number): void;
  scrollTo(x: number, y: number): void;
  scrollToX(x: number): void;
  scrollToY(y: number): void;
  scrollToTop(): void;
  scrollToBottom(): void;
  scrollX(): number;
  scrollY(): number;
  maxScrollX(): number;
  maxScrollY(): number;
  canScrollVertically(): boolean;
  canScrollHorizontally(): boolean;
  contentWidth(): number;
  contentHeight(): number;
  viewportWidth(): number;
  viewportHeight(): number;
  /** 0=Vertical 1=Horizontal 2=Both(与 setScrollDirection 对齐) */
  setScrollDirection(direction: number): void;
  /** 策略: 0=AsNeeded 1=AlwaysOff 2=AlwaysOn(见 C++ JZScrollArea::ScrollBarPolicy) */
  setScrollBarPolicy(policy: number): void;
  setVerticalScrollBarPolicy(policy: number): void;
  setHorizontalScrollBarPolicy(policy: number): void;
}

// 图片显示(缩放倍率 / 旋转角度)
/** @final */
declare class JZImageLabel extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 传 null 抛异常 */
  setImage(image: JZImage): void;
  image(): JZImage;
  setScale(zoom: number): void;
  /** 角度制 */
  setRotation(angle: number): void;
}

// 未暴露(C++ 有): JZScrollArea 的 viewport() 与 verticalScrollBar()/horizontalScrollBar()
//   (视口与滚动条都是 C++ 内部造的子控件, JS 侧没有它们的包装; 要看/改滚动位置请用
//    scrollX/Y()、scrollTo()/scrollToX/Y/Top/Bottom、maxScrollX/Y()、canScrollXxx());
//   JZTableWidget 的 item(row,col)/takeItem/row(item)/currentItem/setCurrentItem(那是 item 句柄接口,
//   按定稿不给 JS —— 要数据用上面的索引式值接口)、JZSplitter.setSizes / sizes。
