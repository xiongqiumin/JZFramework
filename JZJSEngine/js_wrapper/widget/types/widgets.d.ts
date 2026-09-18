// JZWidget 及基础控件 —— 全局类(JZJSWidgetModule::registWidgets +
// jzRegistWidgetJsConstructors 换上的 C 构造器)。
//
// 构造: 与 C++ 构造一致(见本目录 index.d.ts 的"实参形态"清单), 父控件可传 null/undefined;
// 父子关系也可事后用 setParentWidget() / 布局 addWidget() / JZWindow.addWidget() 建立。
//
// 事件走**属性写法**(不是 on()/addListener): 在实例上赋 onPaint / onMousePress / ... ; 清空写 null。
// **信号也是属性**(onClicked / onToggled / onValueChanged / onCurrentIndexChanged ...):
//   `btn.onClicked = () => {...}` 赋值即设、覆盖上一次、`= null` 清空、读回拿到你赋进去的那个值。
//   **赋值不校验**: 赋非函数不会抛, 派发时按"这一项没有回调"处理(回退 C++ 默认实现)。
// 框架派发时看该控件"这一项"有没有回调: 有就转给 JS 并把事件**拦下**, 没有则回退 C++ 默认实现。
// 回调就是**实例上的普通属性**(现读): 所以它与包装对象绑在一起 —— 包装被 GC 掉(没人保活了)
//   回调也就没了。保活由 `@jz/widget` 的对象树负责(见下面的引用相等性说明)。
// 回调里拿"当前控件": 写成箭头函数时没有自己的 this, 用引擎全局 self(), 例:
//   pane.onPaint = (e) => { const p = new JZPainter(self()); };  (见 types/globals.d.ts)
//
// ⚠ 空指针返回值: parentWidget() / parent() / layout() 之类在 C++ 侧没有对象时返回 null。
// ⚠ 引用相等性: 没有"引擎侧身份缓存"(2026-09-17 随旧包装层删掉了) —— 一个底层对象对应哪个 JS 对象,
//   完全由**谁登记**决定。JS 侧登记的关系(setParent / layout.addWidget / window.addWidget /
//   setCentralWidget / addTab / setWidget / 构造时带父 ...)由 `@jz/widget` 的对象树维护, 拿回的就是
//   你当初传进去的那份包装(`===` 成立, 保活也靠这份强引用); layout() 返回缓存那份, 也是同一个。
//   需要"回吐控件指针"的 getter 分两类:
//     * 容器里的子控件(Stacked/Tab 的 widget(i)/currentWidget()、Splitter 的 widget(i)、
//       ScrollArea 的 widget()): 答案是 **JS 侧的对象树**给出来的 —— 拿回的就是你当初 addXxx 传进去
//       的那份包装(`===` 成立, 保活也是那份);
//     * C++ 内部自建的控件(JZGroupBox.contentWidget()、JZScrollArea 的 viewport()/滚动条):
//       JS 侧根本没有它们的对象, 也**不做 C++ 中转**, 所以不提供 —— 需要就直接在 C++ 侧用。
//   值类型(geometry()/rect()/size())每次现造, 比内容用 equal()。
// ⚠ 别把状态挂在包装对象上: 包装是**纯句柄** —— `w.foo = 1` 这类写法 TS 下本来就编译不过(这些类的
//   声明里没有索引签名, 也不打算加)。你自己**长期持有的那份包装**身份是稳定的, 所以以它为键的
//   WeakMap(运行期对象树就是这么记父子/保活的)可靠; 挂在包装上的字段一样会丢。
//   要 per 控件状态: 用稳定键(objectName / 你自己持有的那份包装)自建表, 或放 C++ 侧绑定层。
// ⚠ 布局的生命周期(谁建谁拥有): JS 侧 `new JZVBoxLayout()` 那份归 JS, 控件只是引用它 ——
//   clearLayout() / 控件析构都只解绑, 不 delete(所以手动 setLayout 的布局可以在之后继续用);
//   C++ 内部自建的布局(如 JZWindow.addWidget 按需补的 VBox)归控件, 由控件回收。
//   布局(JZLayout)也继承 JZObject ⇒ 它的 JS 句柄带**守卫**: 万一布局被 C++ 先删掉(或不是 JS 那份
//   归 JS 的), 再用那个句柄会抛"对象已被销毁", 不会踩悬垂指针。

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
  /** 传 null 表示脱离父树(不再被父节点保活) */
  setParentWidget(parent: JZWidget | null): void;
  /** setParentWidget 的同义写法(由 @jz/widget 运行期提供, 见下) */
  setParent(parent: JZWidget | null): void;
  parentWidget(): JZWidget | null;
  /** parentWidget() 的同义写法(由 @jz/widget 运行期提供) */
  parent(): JZWidget | null;
  /** JS 侧登记过的**直接**子控件(由 @jz/widget 运行期提供; 只登记得到经 JS 建立的关系) */
  children(): JZWidget[];
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
   * 之后 `w.layout()` 返回的是**缓存视图**(同一个 JS 对象); 布局本身归 JS(`v` 还能继续用 ——
   * clearLayout() 只是解绑, 不会 delete 它)。
   */
  setLayout(layout: JZLayout): void;
  /**
   * 清空布局(解绑 + 之后 layout() 为 null)。JS 侧 new 出来的布局归 JS, 这里**不删**它
   * (C++ 内部自建的布局会随控件回收); `layout()` 之前发出去的视图仍是同一个对象, 可继续用。
   */
  clearLayout(): void;
  layout(): JZLayout | null;

  /**
   * 右键菜单策略。触发时机是**右键释放**(与 Windows/Qt 默认一致), 且处理权沿父链向上递交:
   * 策略为 NoContextMenu(默认, 即"本控件没有菜单")时交给父控件 —— 于是给容器设一次
   * CustomContextMenu 就能覆盖它里面所有控件; PreventContextMenu 则就地吞掉。
   * 取值: JZWidget.NoContextMenu / PreventContextMenu / DefaultContextMenu /
   *       ActionsContextMenu / CustomContextMenu
   */
  setContextMenuPolicy(policy: number): void;
  contextMenuPolicy(): number;
  /**
   * 挂一份右键菜单 —— 只有策略为 ActionsContextMenu 时由框架自动弹出(锚点即鼠标位置)。
   * **借用**: 控件不负责它的存活(C++ 也不替它保活), 菜单对象被 GC 这里就自然失效
   * (contextMenu() 转 null); 传 null 解挂。想长期有效就自己留引用, 例如 w.setProperty('menu', m)。
   */
  setContextMenu(menu: JZMenu | null): void;
  contextMenu(): JZMenu | null;
  /** 程序化触发同一套流程(坐标是本控件局部坐标; AI / 测试用), 等价于右键点了这一下 */
  requestContextMenu(x: number, y: number): void;

  // ---- 提示 / 样式表 ----
  setTooltip(text: string): void;
  tooltip(): string;
  /** false = CSS 有语法错误(原因只能从 C++ 侧 styleSheetError() 取) */
  setStyleSheet(css: string): boolean;
  styleSheet(): string;

  /**
   * 通用属性 —— **纯 JS 侧存储**(@jz/widget 的 props.ts 用 WeakMap 挂在 JZWidget 原型上),
   * 不经过 C++: 想往控件上挂什么就挂什么。
   *   值: null/undefined 表示**移除**该属性; 其余值(标量 / 数组 / 对象 / 函数 / 控件实例)
   *   一律**原样**存 —— 读回来是同一个对象(身份保留)。
   *   控件被回收时它的属性一起消失; 属性不会额外保活控件本身。
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
  /**
   * 右键菜单请求(策略为 CustomContextMenu 时才有)。x / y 是**接收请求的那个控件**的局部坐标
   * —— 最常见用法: `new JZPoint(x, y)` 经 `this.mapToGlobal(...)` 转成屏幕坐标后交给 menu.popup()。
   */
  onCustomContextMenuRequested?: (x: number, y: number) => void;

  // ---- 右键菜单策略常量(C++ 侧挂在**构造器**上, 子类也能取到: JZLabel.CustomContextMenu) ----
  static readonly NoContextMenu: number;
  static readonly PreventContextMenu: number;
  static readonly DefaultContextMenu: number;
  static readonly ActionsContextMenu: number;
  static readonly CustomContextMenu: number;
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
  /** 取回中央控件(没设过 -> null) */
  centralWidget(): JZWidget | null;
  /** 设置顶部菜单栏(始终排在最顶行); 重复调用替换旧的 */
  addMenuBar(menuBar: JZMenuBar): void;
  /** 取回菜单栏(没设过 -> null) */
  menuBar(): JZMenuBar | null;
}

// 菜单栏(水平排列若干菜单入口)。uiLoader 把它作为 JZMainWindow 的 <widget> 子节点(addMenuBar)。
/** @final */
declare class JZMenuBar extends JZWidget {
  constructor();
  constructor(parent: JZWidget | null);

  /**
   * 添加一个菜单, 返回 JZMenu(**借用句柄**: 菜单栏持有, JS 不负责 delete)。
   * @jz/widget 的 menus.ts 会在 JS 侧保活这个句柄 —— 否则它被 GC 时会按守卫约定把真菜单删掉。
   */
  addMenu(title: string): JZMenu;
  /** 第 i 个菜单(借用句柄, 同上) */
  menuAt(index: number): JZMenu | null;
  menuCount(): number;
}

/**
 * 菜单(数据模型; 不是渲染树控件)。菜单树用 addAction/addMenu/addSeparator 建,
 * 要弹出来用 popup(owner, anchorRect)。
 * ⚠ addAction/addMenu/actionAt/menuAt 返回的都是**借用句柄**(菜单持有) —— JS 侧由
 *   @jz/widget 的 menus.ts 保活。
 */
/** @final */
declare class JZMenu {
  constructor();
  constructor(title: string);

  setTitle(title: string): void;
  title(): string;

  /** 建一个菜单项(借用句柄), 触发时跑它自己的 onTriggered */
  addAction(text: string): JZAction;
  /** 建子菜单(借用句柄, 由本菜单持有) */
  addMenu(title: string): JZMenu;
  addSeparator(): void;

  actionCount(): number;
  actionAt(index: number): JZAction | null;
  menuCount(): number;
  menuAt(index: number): JZMenu | null;

  // ---- 下标式的便捷形式(不拿句柄时用; 触发回调报下标) ----
  setActionEnabled(index: number, enabled: boolean): void;
  setActionChecked(index: number, checked: boolean): void;
  isActionChecked(index: number): boolean;
  /** 某一项被触发(回调参数 = 该项下标) */
  onActionTriggered: ((index: number) => void) | null;

  /**
   * 弹层: anchor 是**屏幕逻辑坐标**下的触发区(菜单在其下沿展开), 传 (globalX, globalY, 1, 1) 即可。
   * 注意它**不是**同步的 exec(): 弹层是异步的, "选中了哪一项"由 onActionTriggered / action.onTriggered 告诉你。
   */
  popup(owner: JZWindow, anchor: JZRect): void;
  hidePopup(): void;
  isPopupOpen(): boolean;
}

/** 菜单项(数据模型) */
/** @final */
declare class JZAction {
  setText(text: string): void;
  text(): string;
  setShortcut(shortcut: string): void;
  shortcut(): string;
  setEnabled(enabled: boolean): void;
  isEnabled(): boolean;
  setCheckable(checkable: boolean): void;
  isCheckable(): boolean;
  setChecked(checked: boolean): void;
  isChecked(): boolean;
  setIcon(icon: JZImage): void;
  /** 触发一次(等价"用户点了这一项") */
  trigger(): void;
  /** 被触发(属性形: 赋值设 / 覆盖旧的 / `= null` 清空) */
  onTriggered: (() => void) | null;
}

/** @final */
declare class JZLabel extends JZWidget {
  /** 运行期形态只有 (text?, parent?): 文字在前, parent 放最后(parent 只能跟在文字后面) */
  constructor(text?: string, parent?: JZWidget | null);

  setText(text: string): void;
  text(): string;
  /** 0=左 1=中 2=右(未挂常量) */
  setAlign(align: number): void;
}

/** @final */
declare class JZPushButton extends JZWidget {
  /**
   * 运行期形态: () / (text) / (text, parent) —— **文字在前, parent 放最后**。
   * 构造不收尺寸: 要固定大小就用 setFixedSize(w, h)(或 setWidth/setHeight)。
   */
  constructor(text?: string, parent?: JZWidget | null);

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
