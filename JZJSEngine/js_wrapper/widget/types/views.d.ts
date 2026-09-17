// 容器 / 视图类控件 —— 全局类(JZJSWidgetModule::registViews)。
//
// 这些控件的数据/子项由 C++ 持有; JS 侧既有"值级"操作(文本、行数、当前项、滚动位置),
// 也能用 item 对象(见 items.d.ts 的 JZTableWidgetItem / JZListWidgetItem)。
//
// 生命周期(与 JZWidget 同一套, 见 index.d.ts 的"生命周期 / 包活"): JS 包装归 GC, 挂进容器时
// 由 `@jz/widget` 运行期登记**保活**(容器持子控件的强引用, 子控件弱引用容器)。
// addWidget / insertWidget / addTab / setWidget 这些入口都已包好, 传进去的控件不需要自己 keep。

// 列表(选页 / 选择用)。注意与 Qt 一样**没有** removeItem 的"摘除"语义:
// 绑定层的 removeItem(i) = takeItem(i) + delete, 确实会释放该项。
/** @final */
declare class JZListWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  appendItem(text: string): void;
  appendItems(items: string[]): void;
  insertItem(index: number, text: string): void;
  insertItem(index: number, item: JZListWidgetItem): void;
  /** 摘除并**释放**第 index 项 */
  removeItem(index: number): void;
  clear(): void;
  count(): number;
  itemText(index: number): string;
  itemData(index: number): any;
  currentIndex(): number;
  setCurrentIndex(index: number): void;
  setItemHeight(h: number): void;

  // ---- item 对象形态(Qt: addItem / item / takeItem / row / currentItem) ----
  /** Qt: addItem(QString) / addItem(QListWidgetItem*) */
  addItem(item: JZListWidgetItem | string): void;
  /** 借用句柄; 越界返回 null。⚠ 该项被 takeItem/removeItem/clear 掉之后不要再碰 */
  item(index: number): JZListWidgetItem | null;
  /** 所有权交回 JS */
  takeItem(index: number): JZListWidgetItem | null;
  /** 未找到返回 -1 */
  row(item: JZListWidgetItem): number;
  currentItem(): JZListWidgetItem | null;
  setCurrentItem(item: JZListWidgetItem | null): void;

  /** 当前项索引变化(属性形) */
  onCurrentIndexChanged: ((index: number) => void) | null;
  /** 激活(点击项 / Enter)(属性形) */
  onActivated: ((index: number) => void) | null;
}

// 表格 —— 值级单元格接口(setItemText / itemText 会自动建 / 取单元格)
/** @final */
declare class JZTableWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  setRowCount(n: number): void;
  rowCount(): number;
  setColumnCount(n: number): void;
  columnCount(): number;
  setItemText(row: number, col: number, text: string): void;
  itemText(row: number, col: number): string;
  insertRow(row: number): void;
  removeRow(row: number): void;
  insertColumn(col: number): void;
  removeColumn(col: number): void;
  clearContents(): void;
  setColumnWidth(col: number, w: number): void;
  /** ⚠ 行高是**整表统一**的(不是按行) */
  setRowHeight(h: number): void;
  rowHeight(): number;
  setHorizontalHeaderLabels(labels: string[]): void;
  setCurrentCell(row: number, col: number): void;
  currentRow(): number;
  currentColumn(): number;

  // ---- item 对象形态(Qt: setItem / item / takeItem / currentItem) ----
  /** 交给表格接管(所有权翻转); 传 null 清空该格 */
  setItem(row: number, col: number, item: JZTableWidgetItem | null): void;
  /** 借用句柄; 越界或空单元格返回 null。⚠ 被 takeItem/removeRow/removeColumn/clearContents 掉之后不要再碰 */
  item(row: number, col: number): JZTableWidgetItem | null;
  /** 所有权交回 JS */
  takeItem(row: number, col: number): JZTableWidgetItem | null;
  /** 未找到返回 -1 */
  row(item: JZTableWidgetItem): number;
  column(item: JZTableWidgetItem): number;
  currentItem(): JZTableWidgetItem | null;
  setCurrentItem(item: JZTableWidgetItem | null): void;

  /** 当前行变化(属性形) */
  onCurrentRowChanged: ((row: number) => void) | null;
}

// 页容器: 多个 page 叠放, 同一时刻只显示当前 page
/** @final */
declare class JZStackedWidget extends JZWidget {
  constructor(parent?: JZWidget | null);

  /** 返回新页下标; page 挂进本容器(所有权转移 + 保活) */
  addWidget(page: JZWidget): number;
  count(): number;
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
  count(): number;
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
  widget(): JZWidget | null;
  /** 虚拟内容总尺寸(w/h < 0 表示该方向跟随视口) */
  setContentSize(w: number, h: number): void;
  scrollTo(x: number, y: number): void;
  scrollX(): number;
  scrollY(): number;
  viewportWidth(): number;
  viewportHeight(): number;
  /** 内部真实滚动条(恒存在, 可见性由策略决定) */
  verticalScrollBar(): JZScrollBar;
  horizontalScrollBar(): JZScrollBar;
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

// 未暴露(C++ 有): JZScrollArea 的滚动条策略 / viewport() / maxScrollX|Y / scrollToTop|Bottom;
// 视图类的 JZTableWidget 列宽读取、JZTabWidget.removeTab / widget(i) / indexOf、
// JZStackedWidget.insertWidget / removeWidget / widget(i)、JZSplitter.setSizes / sizes、
// JZImageLabel.setPivot。
