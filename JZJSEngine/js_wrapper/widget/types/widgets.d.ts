// JZWidget 及常用控件 —— 全局类(JZJSWidgetModule::registWidgets)。
//
// 构造: 除 JZWindow(可传 w/h/title/parent)外都只有**无参**构造 —— 绑定层用默认工厂
// `new Class()` 忽略实参(见 geometry.d.ts 顶部说明)。父子关系用 setParentWidget() /
// JZWindow.addWidget() / 布局 addWidget() 建立, 尺寸用 setGeometry()。
//
// 事件走**属性写法**(不是 on()/addListener): 在实例上赋 onPaint / onMousePress / ... ;
// 框架派发时发现同名的 JS 方法就转给 JS, 否则回退 C++ 默认实现。
// ⚠ 现状: 事件转发只对 **JZWidget / JZWindow** 生效 —— 它们是唯一经 JSWidgetClass 包装创建的
//   (registWidgets 里 setCreate), 其它控件走默认工厂, 是"裸" C++ 对象: 给它们赋 onXxx 不会
//   报错但**静默无效**(按钮的点击请用 onClicked 这类信号接口)。声明仍放在基类上, 因为这是
//   设计上的契约(见本目录 widget.d.ts 的构建器与 qtcompat.js 的用法)。
//
// ⚠ 空指针返回值: parentWidget() / window() / layout() 在 C++ 侧没有对象时返回**真 null**
//   (不再是"空包装对象"那种真值, `if (p)` 判得出空)。
// ⚠ 引用相等性: layout() 走缓存(同一控件 + 同一布局 ⇒ 同一个 JS 对象); 而 parentWidget() /
//   window() 每次都会新建一个包装对象 —— 别用 `===` 比较, 要比就比 objectName() 之类的属性。
// ⚠ 布局的生命周期: 布局归控件所有、由控件负责 delete。layout() 返回的视图在 clearLayout()
//   之后会被置失效(再调用抛异常); 但你交给 setLayout 的那份包装**不会**被标记失效 ——
//   布局被删之后不要再继续用那个变量。

declare class JZWidget {
  constructor();

  // ---- 几何 ----
  move(x: number, y: number): void;
  x(): number;
  y(): number;
  width(): number;
  height(): number;
  resize(w: number, h: number): void;
  setGeometry(x: number, y: number, w: number, h: number): void;
  /** 当前几何(拷贝) */
  geometry(): JZRect;
  setFixedSize(w: number, h: number): void;
  setFixedWidth(w: number): void;
  setFixedHeight(h: number): void;
  setMinimumSize(w: number, h: number): void;
  setMaximumSize(w: number, h: number): void;

  // ---- 显隐 / 焦点 / 重绘 ----
  setVisible(visible: boolean): void;
  isVisible(): boolean;
  show(): void;
  hide(): void;
  setFocus(): void;
  hasFocus(): boolean;
  update(): void;

  // ---- 对象树 / 布局 ----
  setParentWidget(parent: JZWidget): void;
  parentWidget(): JZWidget | null;
  /**
   * 设置布局。Qt 语义(QWidget::setLayout): 一个控件只能有一个布局, 且**只能设一次** ——
   * 已有布局或传 null 都抛 TypeError。换布局请 `clearLayout()` 之后再 `setLayout(新布局)`。
   * 注: 窗口不再预装默认布局, 所以 `new JZWindow(...); win.setLayout(v)` 是合法的"第一次设置";
   * 想要默认纵向布局直接 `win.addWidget(child)` 即可(内部会按需补一个 VBox)。
   * 之后 `w.layout()` 返回的是**缓存视图**(不是 `v` 本身); 布局被删时 `v` 不会被标记失效 ——
   * 别在 clearLayout() / 控件销毁之后再用 `v`。
   */
  setLayout(layout: JZLayout): void;
  /**
   * 清空布局(删除布局对象, 之后 layout() 为 null), 等价 Qt 的 `delete w->layout()`。
   * `layout()` 之前发出去的视图会立刻失效(再调用抛异常)。
   */
  clearLayout(): void;
  layout(): JZLayout | null;

  // ---- 提示 / 样式表 ----
  setTooltip(text: string): void;
  tooltip(): string;
  /** false = CSS 有语法错误(原因只能从 C++ 侧 styleSheetError() 取) */
  setStyleSheet(css: string): boolean;
  styleSheet(): string;

  // ---- JS 事件回调(属性写法; 未实现则回退 C++ 默认实现) ----
  /** 绘制: 在回调里 `new JZPainter(this)` 画; event.rect() 给出本次重绘区 */
  onPaint?: (event: JZPainterEvent) => void;
  onMousePress?: (event: JZMouseEvent) => void;
  onMouseRelease?: (event: JZMouseEvent) => void;
  onMouseMove?: (event: JZMouseEvent) => void;
  onWheel?: (event: JZMouseEvent) => void;
  onEnter?: (event: JZEvent) => void;
  onLeave?: (event: JZEvent) => void;
  onFocusIn?: (event: JZEvent) => void;
  onFocusOut?: (event: JZEvent) => void;
  onShow?: (event: JZShowEvent) => void;
  onHide?: (event: JZHideEvent) => void;
  /** ⚠ 收到的不是 JZEvent 派生对象(见 events.d.ts) */
  onResize?: (event: JZResizeEvent) => void;
  onKeyPress?: (event: JZKeyEvent) => void;
  onKeyRelease?: (event: JZKeyEvent) => void;
}

// 顶层窗口。构造实参有默认值(w=800, h=600, title='JZWindow'), 这是少数**真的接收实参**的构造
// (registWidgets 里 setCreate); parent 必须是 JZWidget(null/undefined 视为无父)。
declare class JZWindow extends JZWidget {
  constructor(w?: number, h?: number, title?: string, parent?: JZWidget);

  setTitle(title: string): void;
  title(): string;
  show(): void;
  close(): void;
  /** 加进窗口自带的垂直布局(JZVBoxLayout) */
  addWidget(child: JZWidget): void;
}

declare class JZLabel extends JZWidget {
  constructor();

  setText(text: string): void;
  text(): string;
  /** 0=左 1=中 2=右(未挂常量) */
  setAlign(align: number): void;
}

declare class JZPushButton extends JZWidget {
  constructor();

  setText(text: string): void;
  text(): string;
  /** icon 不能传 null(绑定里直接解引用) */
  setIcon(icon: JZImage): void;
  icon(): JZImage;
  setWidth(w: number): void;
  setHeight(h: number): void;
  /** 点击回调(sigClicked 转接; 只能添加, 没有对应的解除接口) */
  onClicked(cb: () => void): void;
}

declare class JZLineEdit extends JZWidget {
  constructor();

  setText(text: string): void;
  text(): string;
  setPassword(on: boolean): void;
  isPassword(): boolean;
  setMaxLength(n: number): void;
  maxLength(): number;
  setReadOnly(on: boolean): void;
  isReadOnly(): boolean;
  setPlaceholderText(text: string): void;
  placeholderText(): string;
}

declare class JZCheckbox extends JZWidget {
  constructor();

  setText(text: string): void;
  text(): string;
  setChecked(on: boolean): void;
  isChecked(): boolean;
}

declare class JZRadioButton extends JZWidget {
  constructor();

  setText(text: string): void;
  text(): string;
  setChecked(on: boolean): void;
  isChecked(): boolean;
}

declare class JZSlider extends JZWidget {
  constructor();

  setRange(min: number, max: number): void;
  setValue(v: number): void;
  value(): number;
}

declare class JZProgressBar extends JZWidget {
  constructor();

  setRange(min: number, max: number): void;
  setValue(v: number): void;
  value(): number;
}

declare class JZComboBox extends JZWidget {
  constructor();

  addItem(text: string): void;
  /** ⚠ addItems(数组) 未暴露: 绑定写成 std::vector<std::string> 形参, 而 JS 数组过不了引擎的
   *  形参类型校验(见模块内 JZChart 用 JSValue 形参的原因) —— 目前只能用 addItem 逐个加。 */
  removeItem(index: number): void;
  clear(): void;
  currentIndex(): number;
  setCurrentIndex(index: number): void;
  currentText(): string;
  setCurrentText(text: string): void;
  itemText(index: number): string;
  count(): number;
}

declare class JZSpinBox extends JZWidget {
  constructor();

  setRange(min: number, max: number): void;
  setMinimum(v: number): void;
  minimum(): number;
  setMaximum(v: number): void;
  maximum(): number;
  setStep(v: number): void;
  step(): number;
  setValue(v: number): void;
  value(): number;
  setSuffix(suffix: string): void;
  suffix(): string;
  setReadOnly(on: boolean): void;
  isReadOnly(): boolean;
}

declare class JZGroupBox extends JZWidget {
  constructor();

  setTitle(title: string): void;
  title(): string;
  /** 装载子控件的内容区 */
  contentWidget(): JZWidget | null;
  setLayout(layout: JZLayout): void;
}

// 简易图表: 折线 / 柱状 / 饼图(JZChart)。
declare class JZChart extends JZWidget {
  constructor();

  /** 与 C++ 枚举同值, 挂在构造器上(唯一挂了类型常量的类) */
  static readonly LineChart: 0;
  static readonly BarChart: 1;
  static readonly PieChart: 2;

  // ---- 类型 ----
  setChartType(type: 0 | 1 | 2): void;
  chartType(): 0 | 1 | 2;

  // ---- 序列 ----
  /** 返回新序列下标; color 传 '' 取默认调色板 */
  addSeries(name: string, color: string): number;
  setSeriesData(index: number, data: number[]): void;
  setSeriesName(index: number, name: string): void;
  setSeriesColor(index: number, color: string): void;
  clearSeries(): void;
  seriesCount(): number;
  /** 下标越界抛异常(基类 C++ 不做边界检查, 由绑定层拦) */
  seriesName(index: number): string;
  seriesColor(index: number): JZColor;
  seriesData(index: number): number[];
  /** 一次性设置多条序列, 名称/配色自动生成 */
  setData(seriesList: number[][]): void;

  // ---- 分类标签(折线/柱状的 X 轴; 饼图的扇区名) ----
  setCategories(categories: string[]): void;
  categories(): string[];

  // ---- 外观 ----
  setTitle(title: string): void;
  title(): string;
  setShowGrid(on: boolean): void;
  setShowLegend(on: boolean): void;
  setShowAxis(on: boolean): void;
  setShowPoint(on: boolean): void;
  setShowValue(on: boolean): void;
}

// 未暴露(C++ 有, 需要时按 JZChart 的做法补): JZWidget.rect / pos / size / window / grab /
// minimumWidth|Height / maximumWidth|Height / setSizePolicy / style / setCursor / setOpaque /
// setClipChildren / setStyleClass / styleSheetError; 控件类里 JZListWidget / JZTableWidget /
// JZMenu(JZMenuBar) / JZSplitter / JZTabWidget / JZScrollArea / JZTreeWidget / JZSwitch 等。
