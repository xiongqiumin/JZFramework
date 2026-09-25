// computeruse 包 —— 类型契约（与实现解耦）。
//
// 坐标一律归一化 [0,1]，相对"截图 / 虚拟桌面"。
// C++ 后端(JZJSComputerUseModule)只提供底层语义; 控件树与 UI 操作全在 JS 侧。

/** 归一化矩形（相对截图，[0,1]）。 */
export interface Rect { x: number; y: number; w: number; h: number; }

/** 归一化点。 */
export type Point = { x: number; y: number };

/** 鼠标按键。 */
export type MouseBtn = 'left' | 'right' | 'middle';

/**
 * 语义节点"存在性"的建模期标注（可选）：
 *   - 'must'    建模期确定存在，不存在即视为异常（默认）。
 *   - 'maybe'   可能存在，不存在时静默跳过（如可选面板）。
 *   - 'optional'存在与否都接受。
 */
export type Presence = 'must' | 'maybe' | 'optional';

/**
 * 定位策略：录制期决定"这个控件靠什么被找到"。
 *   ocr    基本文字按钮（OCR 找文字，最省事）
 *   image  图标按钮（模板匹配，单尺度，当前不做多尺度）
 *   color  颜色块（colorFind 按颜色）
 *   yolo   复杂图标（小检测模型，几个样本即可训练）
 * region 为可选的小搜索区域（归一化）；不传则搜整屏。多尺度不在范围内。
 */
export type Locator =
  | { kind: 'ocr';   text: string; region?: Rect }
  | { kind: 'image'; image: string; region?: Rect }
  | { kind: 'color'; color: string; region?: Rect; tol?: number }
  | { kind: 'yolo';  model: string; label?: string; region?: Rect };

/** 探索/录制阶段的配置（实现期再细化字段）。 */
export interface ExploreOptions {
  name: string;
  screenshot?: { maxDim?: number };
  anchor?: { pad?: number };
}

/** 模型序列化快照（固化到磁盘的格式，实现期定字段）。 */
export interface ModelSnapshot {
  name: string;
  nodes: unknown[];
}

// ————————————————————————————————————————————————
// 底层动作后端：由运行期 JZJSComputerUseModule 注入 __jz.computeruse。
// ————————————————————————————————————————————————
export interface ComputerUseBackend {
  /** 按定位策略在当前(小)区域内查找，返回命中矩形；找不到返回 null。单尺度。 */
  match(locator: Locator): Promise<Rect | null>;
  /** 在 rect 中心注入一次鼠标点击；返回是否成功。 */
  click(rect: Rect, btn?: MouseBtn): Promise<boolean>;
  /** 在归一化坐标 from→to 之间拖拽。 */
  drag(from: Point, to: Point): Promise<boolean>;
  /** 按名称按一个键（如 'Return' / 'Tab' / 'LWin' / 'Ctrl+A'）。 */
  key(name: string): Promise<void>;
  /** 输入一段文本。 */
  type(text: string): Promise<void>;
  /** 等待毫秒。 */
  wait(ms: number): Promise<void>;
  /** 虚拟桌面截图 → dataURL（录制/校验用）。 */
  screenshot(): Promise<string>;

  // —— 模型固化 / 装配（JS 侧实现，序列化本树的 JSON）——
  saveModel(app: IApp, path: string): Promise<void>;
  loadModel(path: string): Promise<IApp>;
  explore(opts: ExploreOptions): Promise<IApp>;
  fromPerception(tree: unknown): Promise<IApp>;
}

// ————————————————————————————————————————————————
// 语义节点接口
// ————————————————————————————————————————————————
export interface ICuNode {
  readonly id: string;
  readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  presence?: Presence;
  parent?: ICuNode;
  readonly children: ICuNode[];

  activate(): Promise<void>;
  ensureActivated(): Promise<void>;
  /** 解析为屏幕真实坐标：优先 locator，其次 rect。 */
  locate(): Promise<Rect | null>;
  exists(): Promise<boolean>;

  addTab(label: string, locator?: Locator): ITab;
  addMenu(label: string, locator?: Locator): IMenu;
  addToolbar(label: string, locator?: Locator): IToolbar;
  addGroup(label: string, locator?: Locator): IGroup;
  addWindow(label: string, locator?: Locator): IWindow;
  addCanvas(label: string, locator?: Locator): ICanvas;
  addButton(label: string, locator: Locator): IButton;
  addEdit(label: string, locator: Locator): IEdit;
  addComboBox(label: string, locator: Locator): IComboBox;
  addCheckBox(label: string, locator: Locator): ICheckBox;
  addLabel(label: string, locator?: Locator): ILabel;
  addSlider(label: string, locator: Locator): ISlider;
  addLink(label: string, locator: Locator): ILink;
}

export interface IButton extends ICuNode { readonly type: 'button'; click(btn?: MouseBtn): Promise<void>; }
export interface IMenu extends ICuNode { readonly type: 'menu'; }
export interface ITab extends ICuNode { readonly type: 'tab'; }
export interface IToolbar extends ICuNode { readonly type: 'toolbar'; }
export interface IGroup extends ICuNode { readonly type: 'group'; }
export interface IWindow extends ICuNode { readonly type: 'window'; }
export interface ILabel extends ICuNode { readonly type: 'label'; }

export interface ICanvas extends ICuNode {
  readonly type: 'canvas';
  drag(from: Point, to: Point): Promise<void>;
}

export interface IEdit extends ICuNode {
  readonly type: 'edit';
  setText(text: string): Promise<void>;
  clear(): Promise<void>;
}

export interface IComboBox extends ICuNode {
  readonly type: 'combobox';
  select(label: string): Promise<void>;
}

export interface ICheckBox extends ICuNode {
  readonly type: 'checkbox';
  toggle(): Promise<void>;
  check(): Promise<void>;
  uncheck(): Promise<void>;
}

export interface ISlider extends ICuNode {
  readonly type: 'slider';
  /** f ∈ [0,1]，按水平滑块比例定位（垂直滑块后续补）。 */
  setFraction(f: number): Promise<void>;
}

export interface ILink extends ICuNode { readonly type: 'link'; click(btn?: MouseBtn): Promise<void>; }

// 浏览器/网页元素不单独建模：复用普通节点的 image/ocr/color/yolo 定位即可，
// 这是附带能力，不需要 Browser/WebElement 这类独立控件。
export interface IApp extends ICuNode {
  readonly type: 'app';
  find(type: string, label: string): ICuNode | undefined;
  button(label: string): IButton;
  save(path: string): Promise<void>;
}
