// 列表 / 表格的**子项** —— 全局类(JZJSWidgetModule::registItems)。
//
// ============================ 所有权(三态, 与控件进对象树同一套 /Transfer/) ============================
//   * `new JZTableWidgetItem(text?)` / `new JZListWidgetItem(text?)` —— **JS 拥有**, GC 时释放;
//   * 交给容器(setItem / addItem / insertItem)后**所有权归容器**(容器用 unique_ptr 管),
//     自己别再把它当"我拥有的";
//   * item(...) / currentItem() 拿到的是**借用句柄**: 地址稳定到该项被 takeItem / removeRow /
//     removeColumn / clearContents / clear / removeItem 掉为止 —— 那些操作之后**旧句柄不能再碰**
//     (与 C++ 的借用指针同一约束); takeItem 系列则把所有权交回 JS。
//
// ⚠ 每次取 item 都是**一份新包装**(同一个 C++ 对象的两份包装不是同一个 JS 对象) ——
//    判空请用 `=== null`, 不要用 `===` 比实例。
//
// ============================ 数据 ============================
// data / setData 直接收 JS 值(原样持有, 不做有损转换): 想挂什么就挂什么, 取回来还是同一个对象。

/** 表格单元格(Qt: QTableWidgetItem) */
/** @final */
declare class JZTableWidgetItem {
  constructor();
  constructor(text: string);

  text(): string;
  setText(text: string): void;
  icon(): JZImage;
  setIcon(image: JZImage): void;
  data(): any;
  setData(value: any): void;

  /** 未入表返回 -1 */
  row(): number;
  /** 未入表返回 -1 (Qt: 同上) */
  column(): number;
  /** 所属表格; 未入表 / 已被摘除返回 null */
  table(): JZTableWidget | null;
}

/** 列表项(Qt: QListWidgetItem) */
/** @final */
declare class JZListWidgetItem {
  constructor();
  constructor(text: string);

  text(): string;
  setText(text: string): void;
  icon(): JZImage;
  setIcon(image: JZImage): void;
  data(): any;
  setData(value: any): void;

  /** 所属列表; 未加入 / 已被摘除返回 null */
  listWidget(): JZListWidget | null;
}
