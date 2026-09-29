/**
 * computeruse —— 模块类型(单一来源: 与 ../index.ts + ../types.ts 同源)。
 *
 * 裸名用法(具名导入, **没有 default**):
 *   import { App, setBackend } from 'computeruse';
 *
 * 坐标一律**归一化 [0,1]**(相对截图/虚拟桌面); 只有 `rect` 是屏幕/窗口逻辑像素。
 * C++ 后端(JZJSComputerUseModule)只提供底层语义; 控件树与 UI 操作全在本包(JS) 里。
 */

// ————————————————————————————————————————————————
// 基础类型
// ————————————————————————————————————————————————

/** 归一化矩形（相对截图，[0,1]）。 */
export interface Rect { x: number; y: number; w: number; h: number; }
/** 归一化点。 */
export type Point = { x: number; y: number };
/** 鼠标按键。 */
export type MouseBtn = 'left' | 'right' | 'middle';

/**
 * 语义节点"存在性"的建模期标注(可选): 'must'(默认) / 'maybe' / 'optional'。
 */
export type Presence = 'must' | 'maybe' | 'optional';

/**
 * 定位策略(录制/建模期决定"这个控件靠什么被找到"): ocr / image / color / yolo。
 * region 为可选的小搜索区域(归一化); 不传则搜整屏。多尺度不在范围内。
 */
export type Locator =
  | { kind: 'ocr';   text: string; region?: Rect }
  | { kind: 'image'; image: string; region?: Rect }
  | { kind: 'color'; color: string; region?: Rect; tol?: number }
  | { kind: 'yolo';  model: string; label?: string; region?: Rect };

/**
 * VLM 感知后端配置（OpenAI 兼容的**多模态** chat/completions）。
 * 本包不内置地址：由调用方传（ExploreOptions.vlm）或用 setVlmConfig() 设默认值。
 */
export interface VlmOptions {
  /** 完整端点，如 http://host:8080/v1/chat/completions */
  url: string;
  model?: string;
  /** 需要则填（会发 Authorization: Bearer <apiKey>）。 */
  apiKey?: string;
  /** 单次请求超时（ms，默认 600000 —— 本机大模型很慢）。 */
  timeoutMs?: number;
  maxTokens?: number;
  /** 整段替换内置提示词（坐标口径必须是归一化 [x,y,w,h]）。 */
  prompt?: string;
  /** 追加/覆盖请求体字段（如 { chat_template_kwargs: { enable_thinking: false } }）。 */
  extraBody?: Record<string, unknown>;
}

/** 感知到的单个元素（rect 是**归一化**坐标，相对整图）。 */
export interface PerceptionElement {
  /** 已映射到本包词汇（button/label/edit/checkbox/...）。 */
  type: string;
  /** 控件上的可读文字；空串表示没有文字。 */
  text: string;
  rect: Rect;
  /** 感知后端给的原始说法（调试用，如 "text_field"）。 */
  rawType?: string;
}

/** 一次"看图"的结果。 */
export interface Perception {
  /** 窗口标题（从像素里读出来的；读不到为空串）。 */
  title: string;
  elements: PerceptionElement[];
  /** 后端原始回复（调试用）。 */
  raw?: string;
}

/** 探索/录制阶段配置。 */
export interface ExploreOptions {
  name: string;
  screenshot?: { maxDim?: number };
  anchor?: { pad?: number };
  /** 感知后端；不传则用 setVlmConfig() 设的默认值，都没有就抛错。 */
  vlm?: VlmOptions;
}

/** 模型序列化快照(固化到磁盘的格式)。 */
export interface ModelSnapshot {
  name: string;
  nodes: unknown[];
}

export interface NodeOpts { locator?: Locator; rect?: Rect; }

// ————————————————————————————————————————————————
// 底层动作后端: 运行期由 __jz.computeruse 提供; 也可用 setBackend 换成离线/模拟后端。
// ————————————————————————————————————————————————
export interface ComputerUseBackend {
  /** 按定位策略在当前(小)区域内查找，返回命中矩形；找不到返回 null。单尺度。 */
  match(locator: Locator): Promise<Rect | null>;
  /** 在 rect 中心注入一次鼠标点击；返回是否成功。 */
  click(rect: Rect, btn?: MouseBtn): Promise<boolean>;
  /** 在归一化坐标 from→to 之间拖拽。 */
  drag(from: Point, to: Point): Promise<boolean>;
  /** 按名称按一个键(如 'Return' / 'Tab' / 'LWin' / 'Ctrl+A')。 */
  key(name: string): Promise<void>;
  /** 输入一段文本。 */
  type(text: string): Promise<void>;
  /** 等待毫秒。 */
  wait(ms: number): Promise<void>;
  /** 虚拟桌面截图 → dataURL(录制/校验用)。 */
  screenshot(): Promise<string>;
  /** 模型固化 / 装配(JS 侧实现, 序列化本树的 JSON)。 */
  saveModel(app: IApp, path: string): Promise<void>;
  loadModel(path: string): Promise<IApp>;
  /** ⭐ 探索入口: 从屏幕产出语义场景树(被测实现)。 */
  explore(opts: ExploreOptions): Promise<IApp>;
  fromPerception(tree: unknown): Promise<IApp>;
}

/** 当前后端(可用 setBackend 整体替换)。 */
export let backend: ComputerUseBackend;
/** 替换后端(离线/模拟环境、测试夹具的关键注入点)。 */
export function setBackend(b: ComputerUseBackend): void;

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

export interface IApp extends ICuNode {
  readonly type: 'app';
  find(type: string, label: string): ICuNode | undefined;
  button(label: string): IButton;
  save(path: string): Promise<void>;
}

// ————————————————————————————————————————————————
// 节点类
// ————————————————————————————————————————————————
export abstract class CuNode implements ICuNode {
  constructor(label: string, opts?: NodeOpts);
  id: string;
  abstract readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  presence?: Presence;
  parent?: CuNode;
  children: CuNode[];
  protected add<T extends CuNode>(n: T): T;
  append(child: CuNode): CuNode;
  activate(): Promise<void>;
  ensureActivated(): Promise<void>;
  locate(): Promise<Rect | null>;
  exists(): Promise<boolean>;
  click(btn?: MouseBtn): Promise<void>;
  addTab(l: string, o?: Locator): ITab;
  addMenu(l: string, o?: Locator): IMenu;
  addToolbar(l: string, o?: Locator): IToolbar;
  addGroup(l: string, o?: Locator): IGroup;
  addWindow(l: string, o?: Locator): IWindow;
  addCanvas(l: string, o?: Locator): ICanvas;
  addButton(l: string, loc: Locator): IButton;
  addEdit(l: string, loc: Locator): IEdit;
  addComboBox(l: string, loc: Locator): IComboBox;
  addCheckBox(l: string, loc: Locator): ICheckBox;
  addLabel(l: string, o?: Locator): ILabel;
  addSlider(l: string, loc: Locator): ISlider;
  addLink(l: string, loc: Locator): ILink;
}

export class Button extends CuNode implements IButton {
  readonly type: 'button';
  constructor(label: string, locator: Locator);
}
export class Link extends CuNode implements ILink {
  readonly type: 'link';
  constructor(label: string, locator: Locator);
}
export class Tab extends CuNode implements ITab {
  readonly type: 'tab';
  constructor(label: string, locator?: Locator);
  activate(): Promise<void>;
}
export class Menu extends CuNode implements IMenu {
  readonly type: 'menu';
  constructor(label: string, locator?: Locator);
  activate(): Promise<void>;
}
export class Toolbar extends CuNode implements IToolbar { readonly type: 'toolbar'; constructor(l: string, o?: Locator); }
export class Group extends CuNode implements IGroup { readonly type: 'group'; constructor(l: string, o?: Locator); }
export class Window extends CuNode implements IWindow { readonly type: 'window'; constructor(l: string, o?: Locator); }
export class Label extends CuNode implements ILabel { readonly type: 'label'; constructor(l: string, o?: Locator); }
export class Canvas extends CuNode implements ICanvas {
  readonly type: 'canvas';
  constructor(l: string, o?: Locator);
  drag(from: Point, to: Point): Promise<void>;
}
export class Edit extends CuNode implements IEdit {
  readonly type: 'edit';
  constructor(label: string, locator: Locator);
  setText(text: string): Promise<void>;
  clear(): Promise<void>;
}
export class ComboBox extends CuNode implements IComboBox {
  readonly type: 'combobox';
  constructor(label: string, locator: Locator);
  select(label: string): Promise<void>;
}
export class CheckBox extends CuNode implements ICheckBox {
  readonly type: 'checkbox';
  constructor(label: string, locator: Locator);
  toggle(): Promise<void>;
  check(): Promise<void>;
  uncheck(): Promise<void>;
}
export class Slider extends CuNode implements ISlider {
  readonly type: 'slider';
  constructor(label: string, locator: Locator);
  setFraction(f: number): Promise<void>;
}

export class App extends CuNode implements IApp {
  readonly type: 'app';
  constructor(name: string, opts?: NodeOpts);
  find(type: string, label: string): CuNode | undefined;
  button(l: string): IButton;
  save(path: string): Promise<void>;
  static load(path: string): Promise<IApp>;
  static explore(opts: ExploreOptions): Promise<IApp>;
  static fromPerception(tree: unknown): Promise<IApp>;
}

// ————————————————————————————————————————————————
// 底层 primitive(通用 OS 动作, 非树内控件)
// ————————————————————————————————————————————————
export function key(name: string): Promise<void>;
export function type(text: string): Promise<void>;
export function wait(ms: number): Promise<void>;

// ————————————————————————————————————————————————
// 感知层(可换后端: VLM / OCR / 检测 都在 perceive.ts 这一个文件里)
// ————————————————————————————————————————————————
/** 设默认 VLM 后端(调用方不想每次传 ExploreOptions.vlm 时用; null 清除)。 */
export function setVlmConfig(cfg: VlmOptions | null): void;
export function vlmConfig(): VlmOptions | null;
/** 看一张图(dataURL), 返回元素清单。 */
export function perceive(shot: string, opts?: { vlm?: VlmOptions }): Promise<Perception>;
/** 内置提示词(带图片尺寸与算好的归一化例子); 需要改口径时可整段替换。 */
export function buildPrompt(imgW: number, imgH: number): string;
