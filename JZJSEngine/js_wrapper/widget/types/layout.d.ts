// 布局 —— 全局类(JZJSWidgetModule::registWidgets 里的 ClassBind<JZLayout> 与四个具体布局)。
//
// 用法: widget.setLayout(layout); 盒子布局把子控件 addWidget(w, stretch) 加进去。
// 注意 stretch 参数**必填** —— 绑定层按精确参数个数校验(形参表含 this, 见 JZJSEngine::call),
// 少写一个就报"参数错误:需要N个参数"; 不拉伸传 0。
//
// 具体布局都经 ClassBind<Xxx>(name, bind_layout) 注册了基类, 所以 `layout instanceof JZLayout`
// 成立 —— JZWidget.setLayout(JZLayout*) 的参数校验要求这一点。
//
// JZLayout 是抽象基类(纯虚 updateLayout), 注册时没有提供构造函数: `new JZLayout()` 抛异常,
// 请用 JZVBoxLayout / JZHBoxLayout / JZGridLayout / JZSelfLayout。

declare class JZLayout {
  /** 抽象基类, 不能实例化 */
  protected constructor();

  setSpacing(px: number): void;
  spacing(): number;
  /** 内容边距(布局留白) */
  setContentsMargins(left: number, top: number, right: number, bottom: number): void;
}

declare class JZVBoxLayout extends JZLayout {
  constructor();

  /** stretch 必填(不拉伸传 0) */
  addWidget(widget: JZWidget, stretch: number): void;
  addSpacing(px: number): void;
  addStretch(stretch: number): void;
  /** 插入到第 index 个位置; stretch 必填 */
  insertWidget(index: number, widget: JZWidget, stretch: number): void;
  removeAt(index: number): void;
  /** 摘除控件(不删除控件本身) */
  removeWidget(widget: JZWidget): void;
  clear(): void;
  count(): number;
}

declare class JZHBoxLayout extends JZLayout {
  constructor();

  /** stretch 必填(不拉伸传 0) */
  addWidget(widget: JZWidget, stretch: number): void;
  addSpacing(px: number): void;
  addStretch(stretch: number): void;
  /** 插入到第 index 个位置; stretch 必填 */
  insertWidget(index: number, widget: JZWidget, stretch: number): void;
  removeAt(index: number): void;
  removeWidget(widget: JZWidget): void;
  clear(): void;
  count(): number;
}

declare class JZGridLayout extends JZLayout {
  constructor();

  /** 放到第 row 行第 col 列 */
  addWidget(widget: JZWidget, row: number, column: number): void;
  /** 跨 rowSpan × columnSpan */
  addWidgetSpan(widget: JZWidget, row: number, column: number, rowSpan: number, columnSpan: number): void;
  setRowStretch(row: number, stretch: number): void;
  setColumnStretch(column: number, stretch: number): void;
  setRowMinimumHeight(row: number, height: number): void;
  setColumnMinimumWidth(column: number, width: number): void;
  setHorizontalSpacing(px: number): void;
  setVerticalSpacing(px: number): void;
  rowCount(): number;
  columnCount(): number;
  count(): number;
  removeWidget(widget: JZWidget): void;
  clear(): void;
}

declare class JZSelfLayout extends JZLayout {
  constructor();

  /** 九宫格锚点定位: anchor 0..8 = TopLeft TopCenter TopRight / MiddleLeft Center MiddleRight /
   *  BottomLeft BottomCenter BottomRight; dx/dy 为偏移(未挂常量, 直接传数字) */
  addWidget(widget: JZWidget, anchor: number, dx: number, dy: number): void;
  /** 只改锚点/偏移(不重复登记) */
  setAnchor(widget: JZWidget, anchor: number, dx: number, dy: number): void;
  removeWidget(widget: JZWidget): void;
  removeAt(index: number): void;
  clear(): void;
  count(): number;
}

// 未暴露(C++ 有): JZLayout.apply/minimumSizeHint、盒子布局的 spacing() 读写之外的属性,
// JZStackedLayout / JZFormLayout 之类的其余布局类型(若 C++ 侧存在)。
