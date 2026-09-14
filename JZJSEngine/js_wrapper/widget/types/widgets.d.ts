// JZWidget 及基础控件 —— 全局类(JZJSWidgetModule::registWidgets +
// jzRegistWidgetJsConstructors 换上的 C 构造器)。
//
// 构造: 与 C++ 构造一致(见本目录 index.d.ts 的"实参形态"清单), 父控件可传 null/undefined;
// 父子关系也可事后用 setParentWidget() / 布局 addWidget() / JZWindow.addWidget() 建立。
//
// 事件走**属性写法**(不是 on()/addListener): 在实例上赋 onPaint / onMousePress / ... ; 清空写 null。
// **信号也是属性**(onClicked / onToggled / onValueChanged / onCurrentIndexChanged ...):
//   `btn.onClicked = () => {...}` 赋值即设、覆盖上一次、`= null` 清空、读回能拿到当前回调 ——
//   与事件槽**同一条规则**(单发; 见 JZJSWidgetModule 的 defSignalProperty)。不再是方法调用。
// 框架派发时看该控件"这一项"有没有回调: 有就转给 JS 并把事件**拦下**, 没有则回退 C++ 默认实现。
// 回调**存在 C++ 侧**(不是包装对象的属性), 所以与"挂在哪一份包装上"无关: 任何被 JS 包装过的控件
//   (含 C++ 侧 new 好再交给 JS 的控件、groupBox.contentWidget() 这类内部控件)赋 onXxx 都生效;
//   赋 null 清空、赋非函数抛异常。
// 回调里拿"当前控件": 写成箭头函数时没有自己的 this, 用引擎全局 self() —— 等价于这里的 this,
//   例: pane.onPaint = (e) => { const p = new JZPainter(self()); };  (见 types/globals.d.ts)
//   转发状态存在 JZJSWidgetModule 的控件登记节点上(不是控件对象的成员): 模块自己就是这些
//   控件的**事件过滤器**(installEventFilter), 所以 C++ 侧建好再交给 JS 的 JZWidget/JZWindow
//   (如 groupBox.contentWidget()、parentWidget())同样能收到 onXxx, 改父后也照旧。
//
// ⚠ 空指针返回值: parentWidget() / window() / layout() / JZGroupBox.contentWidget() 之类
//   在 C++ 侧没有对象时返回**真 null**(不再是"空包装对象"那种真值, `if (p)` 判得出空)。
// ⚠ 引用相等性: parentWidget() / window() / widget() / contentWidget() / currentWidget() /
//   verticalScrollBar() ... 这些 getter **按当前调用现造包装** —— 同一个控件的两份包装不是同一个
//   JS 对象, 所以 `===` 比不得, 也别把 JS 侧状态(自定义字段)挂在包装上指望"下次还是它"。
//   事件回调不受影响(onPaint 等存在 C++ 侧的槽里, 在哪份包装上赋都生效)。layout() 走缓存, 返回同一份。
// ⚠ 别把状态挂在包装对象上: 包装是**纯句柄** —— `w.foo = 1` 这类写法 TS 下本来就编译不过(这些类的
//   声明里没有索引签名, 也不打算加), 而绕过它(声明合并 / `as any`)之后运行期同样靠不住: getter
//   现造包装, 同一控件的两份包装是两个 JS 对象, 于是"以包装对象为键的 Map/WeakMap"和挂在包装上的
//   字段一样会丢。要 per 控件状态: 用稳定键(objectName / 你自己持有的那份包装)自建表, 或放 C++ 侧绑定层。
// ⚠ 布局的生命周期: 布局归控件所有、由控件负责 delete。layout() 返回的视图在 clearLayout()
//   之后会被置失效(再调用抛异常); 但你交给 setLayout 的那份包装**不会**被标记失效 ——
//   布局被删之后不要再继续用那个变量。

/** @final */
declare class JZWidget {
  /** 无父控件; 也可构造时就带父: new JZWidget(parent) */
  constructor(parent?: JZWidget | null);

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
  /** 自身矩形(控件本地坐标: x/y 恒为 0), 绘制回调里填背景用 */
  rect(): JZRect;
  setFixedSize(w: number, h: number): void;
  setFixedWidth(w: number): void;
  setFixedHeight(h: number): void;
  setMinimumSize(w: number, h: number): void;
  setMaximumSize(w: number, h: number): void;
  /** 尺寸上下限的单项设置(布局里给"固定/伸缩"留余地, 比 setMinimumSize 好用) */
  setMinimumWidth(w: number): void;
  setMinimumHeight(h: number): void;
  setMaximumWidth(w: number): void;
  setMaximumHeight(h: number): void;

  // ---- 显隐 / 焦点 / 重绘 ----
  setVisible(visible: boolean): void;
  isVisible(): boolean;
  show(): void;
  hide(): void;
  setFocus(): void;
  hasFocus(): boolean;
  /** 排一次重绘(合并脏区后走 paintEvent) */
  update(): void;

  // ---- 对象树 / 坐标 ----
  /** 传 null 表示脱离父级(所有权交回 JS, 见模块头的所有权说明) */
  setParentWidget(parent: JZWidget | null): void;
  parentWidget(): JZWidget | null;
  /** 所属顶层窗口; 不在窗口里返回 null(每个调用都是新包装对象, 别用 === 比) */
  window(): JZWindow | null;
  /** parent 为 null 时映射到屏幕坐标 */
  mapTo(parent: JZWidget | null, point: JZPoint): JZPoint;
  mapTo(parent: JZWidget | null, x: number, y: number): JZPoint;

  // ---- 对象名 / 样式 ----
  /** 样式表 #id 选择器、AI 定位、自检都靠它 */
  setObjectName(name: string): void;
  objectName(): string;
  /** 空格分隔的类名串(供 .class 选择器), 如 "btn primary" */
  setStyleClass(cls: string): void;
  styleClass(): string;

  // ---- 布局 ----
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

  /**
   * 通用属性(JZObject::setProperty) —— 存进控件的属性表, 用 property(name) 读回。
   * 子类的"预定义属性"由各自的 override 认名字, 认不出的落到这张通用表。
   * 值: null/undefined(**表示移除该属性**)、number、string、boolean 按各自类型存;
   *   其余值(数组 / 对象 / 函数 / 类的实例)按**原样**存(注册的 metatype JZJSValuePtr, 不做有损转换):
   *   身份保留, 读回来还是同一个对象; 引用计数由引擎管, 存活到该属性被覆盖/移除或引擎关闭。
   */
  setProperty(name: string, value: unknown): void;

  /**
   * 读回 setProperty 存的值。未设置或已移除 -> null。
   * 标量按原类型还(number/string/boolean); 字节串 -> Uint8Array; 不可转的 C++ 侧自定义类型抛异常。
   */
  property(name: string): any;

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
  onResize?: (event: JZResizeEvent) => void;
  onKeyPress?: (event: JZKeyEvent) => void;
  onKeyRelease?: (event: JZKeyEvent) => void;
}

// 顶层窗口。构造实参: () / (parent) / (w, h) / (w, h, title) / (w, h, title, parent)。
// 无参构造的尺寸留 0, 首次 show() 时按 sizeHint 自适应。
/** @final */
declare class JZWindow extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);
  constructor(w: number, h: number);
  constructor(w: number, h: number, title: string);
  constructor(w: number, h: number, title: string, parent: JZWidget | null);

  setTitle(title: string): void;
  title(): string;
  show(): void;
  close(): void;
  /** 加进窗口自带的垂直布局(JZVBoxLayout, 不存在则按需新建) */
  addWidget(child: JZWidget): void;
}

// 主窗口(菜单栏 + 中央控件)。构造实参只有 (parent?) —— 标题/几何走 setTitle/setGeometry。
// 中央控件用 setCentralWidget(至多一个, 占满菜单栏外剩余区域); 菜单栏用 addMenuBar。
/** @final */
declare class JZMainWindow extends JZWindow {
  constructor();
  constructor(parent: JZWidget | null);

  /** 设置中央控件(占用菜单栏外的全部剩余空间); 重复调用替换旧的 */
  setCentralWidget(widget: JZWidget): void;
  /** 设置顶部菜单栏(始终排在最顶行); 重复调用替换旧的 */
  addMenuBar(menuBar: JZMenuBar): void;
}

// 菜单栏(水平排列若干菜单入口)。uiLoader 把它作为 JZMainWindow 的 <widget> 子节点(addMenuBar)。
/** @final */
declare class JZMenuBar extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);

  /** 添加一个菜单, 返回其 JZMenu 指针(继续 addAction / addMenu); 内部菜单项 JZMenu 暂未暴露 */
  addMenu(title: string): any;
}

/** @final */
declare class JZLabel extends JZWidget {
  /** 运行期 (parent) / (text) / (text, parent) 都接受(顺序无关) */
  constructor(text?: string, parent?: JZWidget | null);

  setText(text: string): void;
  text(): string;
  /** 0=左 1=中 2=右(未挂常量) */
  setAlign(align: number): void;
}

/** @final */
declare class JZPushButton extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);
  constructor(parent: JZWidget | null, w: number, h: number, text: string);

  setText(text: string): void;
  text(): string;
  /** icon 不能传 null(绑定里直接解引用) */
  setIcon(icon: JZImage): void;
  icon(): JZImage;
  setWidth(w: number): void;
  setHeight(h: number): void;
  /** 点击回调(属性形: 赋值设, 覆盖旧的, `= null` 清空; 回调里用 self() 取按钮) */
  onClicked: (() => void) | null;
}

/** @final */
declare class JZLineEdit extends JZWidget {
  constructor(parent?: JZWidget | null);

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

  /** 文本变化(属性形: 赋值设 / 覆盖 / `= null` 清空) */
  onTextChanged: ((text: string) => void) | null;
  /** 回车(属性形) */
  onReturnPressed: (() => void) | null;
  /** 失焦 / 回车(属性形) */
  onEditingFinished: (() => void) | null;
}

/** @final */
declare class JZCheckbox extends JZWidget {
  constructor(parent?: JZWidget | null);

  setText(text: string): void;
  text(): string;
  setChecked(on: boolean): void;
  isChecked(): boolean;
  /** 选中变化(属性形); 回调带 (on: boolean) */
  onToggled: ((on: boolean) => void) | null;
}

/** @final */
declare class JZRadioButton extends JZWidget {
  /** 运行期 (parent) / (text) / (text, parent) 都接受(顺序无关) */
  constructor(text?: string, parent?: JZWidget | null);

  setText(text: string): void;
  text(): string;
  setChecked(on: boolean): void;
  isChecked(): boolean;
  /** ⚠ 这条链上的 toggled 是 **无参** 信号(JZSigVoid), 与 JZCheckbox/JZSwitch 不同; 属性形 */
  onToggled: (() => void) | null;
}

/** @final */
declare class JZSlider extends JZWidget {
  constructor(parent?: JZWidget | null);

  setRange(min: number, max: number): void;
  setValue(v: number): void;
  value(): number;
  /** 值变化(属性形); 回调带 (value: number) */
  onValueChanged: ((value: number) => void) | null;
}

/** @final */
declare class JZProgressBar extends JZWidget {
  constructor(parent?: JZWidget | null);

  setRange(min: number, max: number): void;
  setValue(v: number): void;
  value(): number;
}

/** @final */
declare class JZComboBox extends JZWidget {
  constructor(parent?: JZWidget | null);

  addItem(text: string): void;
  /** 数组形参: 绑定层用 JSValue 收(JS 数组过不了引擎的形参类型校验) */
  addItems(items: string[]): void;
  removeItem(index: number): void;
  clear(): void;
  currentIndex(): number;
  setCurrentIndex(index: number): void;
  currentText(): string;
  setCurrentText(text: string): void;
  itemText(index: number): string;
  count(): number;

  /** 当前项索引变化(属性形) */
  onCurrentIndexChanged: ((index: number) => void) | null;
  /** 当前项文本变化(属性形) */
  onCurrentTextChanged: ((text: string) => void) | null;
}

/** @final */
declare class JZSpinBox extends JZWidget {
  constructor(parent?: JZWidget | null);

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
  /** 值变化(属性形); 回调带 (value: number) */
  onValueChanged: ((value: number) => void) | null;
}

/** @final */
declare class JZGroupBox extends JZWidget {
  /** 运行期 (parent) / (title) / (title, parent) 都接受(顺序无关) */
  constructor(title?: string, parent?: JZWidget | null);

  setTitle(title: string): void;
  title(): string;
  /** 装载子控件的内容区(构造时就建好, 恒非空) */
  contentWidget(): JZWidget;
  /** 等价 contentWidget().setLayout(layout) —— 布局应用到内部内容区 */
  setLayout(layout: JZLayout): void;
}

// 开关(on / off)
/** @final */
declare class JZSwitch extends JZWidget {
  constructor(parent?: JZWidget | null);

  setState(on: boolean): void;
  isOn(): boolean;
  /** 选中变化(属性形); 回调带 (on: boolean) */
  onToggled: ((on: boolean) => void) | null;
}

// 多行文本编辑(只读时可直接当滚动日志用)
/** @final */
declare class JZTextEdit extends JZWidget {
  constructor(parent?: JZWidget | null);

  setText(text: string): void;
  text(): string;
  setPlaceholderText(text: string): void;
  placeholderText(): string;
  setReadOnly(on: boolean): void;
  isReadOnly(): boolean;
  setLineWrap(wrap: boolean): void;
  lineWrap(): boolean;
  cursorPosition(): number;
  setCursorPosition(pos: number): void;
  selectAll(): void;
  /** 文本变化(属性形); 回调带 (text: string) */
  onTextChanged: ((text: string) => void) | null;
}

// 简易图表: 折线 / 柱状 / 饼图(JZChart)。颜色形参用 CSS 风格字符串; 传 '' 取默认调色板。
/** @final */
declare class JZChart extends JZWidget {
  constructor(parent?: JZWidget | null);

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

// 未暴露(C++ 有, 需要时按 JZChart 的做法补): JZWidget.pos / size / grab / sizeHint /
// setSizePolicy / style / setCursor / setOpaque / setClipChildren / styleSheetError;
// 也没注册的控件类: JZDialog / JZMessageBox / JZFileDialog / JZMenu /
// JZTreeWidget / JZAnimationGroup / JZPropertyAnimation(容器与视图类见 views.d.ts)。
// (JZMainWindow / JZMenuBar 已注册并暴露, 见上方声明)
