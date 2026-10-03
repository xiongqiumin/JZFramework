/**
 * computeruse —— 模块类型(单一来源: 与 ../index.ts + ../types.ts + ../layout.ts + ../diagnose.ts 同源)。
 *
 * 裸名用法(具名导入, **没有 default**):
 *   import { App, setBackend, diagnose } from 'computeruse';
 *
 * 坐标一律**物理像素**(相对整屏/虚拟桌面; = 截图那张图的像素空间)。不再是归一化 [0,1]。
 * C++ 后端(JZJSComputerUseModule)只提供底层语义; 控件树与 UI 操作全在本包(JS) 里。
 *
 * ⭐ 两条铁律(见 docs/compute-use-design.md):
 *   ① 像素只在**建模期**出现: 建模时产出"像素 ↔ 布局映射"(`layout` 路径);
 *      运行期只按布局路径寻址(`Locator.kind='layout'`), 像素族只在退化兜底时用。
 *   ② 界面不匹配先本地分级(`diagnose`), 稳定复现才升级大模型。
 */

// ————————————————————————————————————————————————
// 基础类型
// ————————————————————————————————————————————————

/** **物理像素**矩形（相对整屏/虚拟桌面；x,y = 左上角）。 */
export interface Rect { x: number; y: number; w: number; h: number; }
/** **物理像素**点。 */
export type Point = { x: number; y: number };
/** 屏幕尺寸/缩放（`backend.screenInfo()`）：脚本查"屏多大/缩放多少"用（不是换算步骤）。 */
export interface ScreenInfo {
  virtual: boolean;
  logical: [number, number];
  physical: [number, number];
  scale: number;
  dpi?: number;
}
/** 鼠标按键。 */
export type MouseBtn = 'left' | 'right' | 'middle';

/** 一个 **JZImage**（widget 包的全局类，`screenshotImage()` 给的就是它）。
 *  本包与 cv 包一样刻意**不 import** widget 的类型（自包含），只声明用得上的那几个方法；
 *  运行期由 C++ 侧按真实类型校验。配合 `cv.fromImage(img)` 使用。 */
export interface JZImageLike {
  width(): number;
  height(): number;
  isNull(): boolean;
}

/**
 * 语义节点"存在性"的建模期标注(可选): 'must'(默认) / 'maybe' / 'optional'。
 */
export type Presence = 'must' | 'maybe' | 'optional';

/** 布局路径：从窗口往下拼出来的稳定标识（如 `/main/btnOutput`）。 */
export type LayoutPath = string;

/**
 * 定位策略(建模期决定"这个控件靠什么被找到"):
 *   layout ⭐ 运行期正路(按布局路径问当前几何, 不涉像素);
 *   ocr / image / color / yolo 是**像素族**，只用于建模期辅助与无布局时的显式兜底。
 * region 为可选的小搜索区域(**物理像素**); 不传则搜整屏。
 */
export type Locator =
  | { kind: 'layout'; path: LayoutPath; region?: Rect }
  | { kind: 'ocr';   text: string; region?: Rect }
  | { kind: 'image'; image: string; region?: Rect }
  | { kind: 'color'; color: string; region?: Rect; tol?: number }
  | { kind: 'yolo';  model: string; label?: string; region?: Rect };

/** 可访问布局树节点（自家 = dumpSemanticTree；外部 = Win32/UIA/MSAA）。 */
export interface LayoutNode {
  role: string;
  id?: string;
  text?: string;
  name?: string;
  /** **物理像素**矩形。 */
  rect?: Rect;
  visible?: boolean;
  enabled?: boolean;
  clickable?: boolean;
  checked?: boolean;
  range?: { value: number; min: number; max: number };
  items?: string[];
  actions?: string[];
  children?: LayoutNode[];
}

/** 页面指纹：窗口标识 + 关键节点路径集合（判"还在不在同一页"）。 */
export interface PageFingerprint { window?: string; paths: LayoutPath[]; }

/** `attach()` 的结果。 */
export interface AttachResult {
  window?: string;
  /** **当前**页面的指纹（模型里记的是"期望指纹"，两者不是一回事）。 */
  fingerprint: PageFingerprint;
  /** 期望指纹与当前页面的吻合度（[0,1]）。 */
  pageMatch: number;
  total: number;
  bound: number;
  degraded: LayoutPath[];
  missing: LayoutPath[];
}

/** 节点当前状态（`read()` 的返回）。 */
export interface NodeState {
  exists: boolean;
  from: 'layout' | 'rect';
  text?: string;
  checked?: boolean;
  enabled?: boolean;
  clickable?: boolean;
  range?: { value: number; min: number; max: number };
  rect?: Rect;
}

/** `expect()` 的期望条件。 */
export type ExpectMatcher =
  | string | RegExp | number | boolean
  | {
      text?: string | RegExp;
      checked?: boolean;
      enabled?: boolean;
      exists?: boolean;
      range?: { value?: number; min?: number; max?: number };
    };

/** `layoutTree()` 参数：指定目标窗口（`title` 空 = 用前台窗口）。 */
export interface LayoutTreeOptions { title?: string; }

/** `sync()` 参数：等界面稳定（像素只当信号）。 */
export interface SyncOptions { settle?: number; timeoutMs?: number; }

/** `launch()` 参数。 */
export interface LaunchOptions {
  exe?: string;
  args?: string[];
  cwd?: string;
  title?: string | RegExp;
  timeoutMs?: number;
}

/** 失败性质 / 本地处置建议。 */
export type FailureKind =
  | 'none' | 'transient' | 'popup' | 'precondition'
  | 'page-miss' | 'path-drift' | 'process-gone' | 'no-layout';
export type FailureAction =
  | 'retry' | 'wait' | 'dismiss-popup' | 'add-precondition'
  | 'ask-model' | 'restart' | 'abort';

/** `diagnose()` 的输入 / 结论。 */
export interface DiagnoseContext {
  attempt?: number;
  maxAttempts?: number;
  fingerprint?: PageFingerprint | null;
  layout?: LayoutNode | null;
  path?: LayoutPath;
  expect?: ExpectMatcher;
}
export interface Diagnosis {
  kind: FailureKind;
  transient: boolean;
  action: FailureAction;
  reason: string;
  evidence: {
    path?: LayoutPath;
    fingerprintChanged?: boolean;
    extraPaths?: string[];
    missingPath?: string;
    foundButUnusable?: string;
  };
}

/** 本包的错误码。 */
export type CuErrorCode =
  | 'not-found' | 'no-layout' | 'expect-failed' | 'window-gone' | 'send-failed' | 'unsupported';

// ————————————————————————————————————————————————
// 运行期修复：界面不匹配 → （只有真错才）升级大模型 → 落 override
// ————————————————————————————————————————————————

/** 升级所需的最小 App 契约（`App` 天然满足）。 */
export interface RepairTarget {
  diagnose(err: unknown, opts?: { attempt?: number; maxAttempts?: number;
                                 path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis>;
  getLayout(): LayoutNode | null;
  override: Record<LayoutPath, LayoutPath>;
  fingerprint?: PageFingerprint;
}

/** 修复用模型端点（OpenAI 兼容 chat/completions）。 */
export interface RepairModelOptions {
  url: string;
  model?: string;
  apiKey?: string;
  timeoutMs?: number;
  maxTokens?: number;
  prompt?: string;
  extraBody?: Record<string, unknown>;
}

export interface RepairOptions extends RepairModelOptions {
  path: LayoutPath;
  expect?: ExpectMatcher;
  attempt?: number;
  maxAttempts?: number;
  step?: string;
  history?: { retries?: number; lastModelAnswer?: string };
}

export interface RepairAnswer { kind?: string; action?: string; newPath?: string; reason?: string; }

export interface RepairResult {
  diagnosis: Diagnosis;
  localOnly: boolean;
  answer: RepairAnswer;
  applied: boolean;
  override?: { from: LayoutPath; to: LayoutPath };
  evidence?: string;
  raw?: string;
}

/**
 * VLM 感知后端配置（OpenAI 兼容的**多模态** chat/completions）。
 * 本包不内置地址：由调用方传（ExploreOptions.vlm）或用 setVlmConfig() 设默认值。
 */
export interface VlmOptions {
  url: string;
  model?: string;
  apiKey?: string;
  timeoutMs?: number;
  maxTokens?: number;
  /** 整段替换内置提示词（要求模型按**像素**给框，相对送出去的图 = 物理像素；不做比例换算）。 */
  prompt?: string;
  extraBody?: Record<string, unknown>;
}

/** 感知到的单个元素（rect 已换算成**物理像素**，与 click/布局同一口径）。 */
export interface PerceptionElement {
  type: string;
  text: string;
  rect: Rect;
  rawType?: string;
}

/** 一次"看图"的结果。 */
export interface Perception { title: string; elements: PerceptionElement[]; raw?: string; }

/** 人审/标注钩子。 */
export type PerceptionReview = (p: Perception) => Perception | Promise<Perception>;

/** 建模(探索)阶段配置。 */
export interface ExploreOptions {
  name: string;
  screenshot?: { maxDim?: number };
  anchor?: { pad?: number };
  vlm?: VlmOptions;
  /** 人审/标注（可选，默认不启用）。 */
  review?: PerceptionReview;
  /** 布局树（建模期用它把感知元素认领到布局节点 ⇒ 生成映射）。 */
  layout?: LayoutNode | null;
}

/** 模型序列化快照。 */
export interface ModelSnapshot { name: string; nodes: unknown[]; }
export interface NodeOpts { locator?: Locator; rect?: Rect; layout?: LayoutPath; }

// ————————————————————————————————————————————————
// 后端
// ————————————————————————————————————————————————

export interface ComputerUseBackend {
  /** 按定位策略查找；运行期应只出现 `kind:'layout'`（不涉像素）。 */
  match(locator: Locator): Promise<Rect | null>;
  click(rect: Rect, btn?: MouseBtn): Promise<boolean>;
  drag(from: Point, to: Point): Promise<boolean>;
  /** **敲一下**（按下 + 抬起），如 'Return' / 'Tab' / 'LWin' / 'Ctrl+A'。
   *  够用于**事件驱动**的消费者（输入框/菜单/按钮：它们在自己的 keyPress/keyRelease 里就处理完）。
   *  ⚠ 对"每个逻辑帧读一次当前按着谁"的应用（游戏循环/时间线定时器），一次敲击**不保证被看见**
   *  （两个事件可能落在同一个逻辑帧之前）—— 那种场景用 keyDown/keyUp 自己控制按住时长。 */
  key(name: string): Promise<void>;
  /** **按住**（键盘状态：按下后一直保持，直到 keyUp）。
   *  应用是按逻辑帧轮询按键的 ⇒ "按住多久"由**调用方**决定，这正是真实键盘的形状（按住就走 / 按住就打）。
   *  ⚠ 与 keyUp 成对使用；收尾时要把按下的键抬起，否则应用会一直以为键还按着（下一个脚本也继承）。
   *  两个都是**投递即返回**（不阻塞调用方的循环）。 */
  keyDown(name: string): Promise<void>;
  /** **抬起**（与 keyDown 成对）。 */
  keyUp(name: string): Promise<void>;
  type(text: string): Promise<void>;
  wait(ms: number): Promise<void>;
  /** 截图 → dataURL。尺寸 = 该后端的**物理像素**（与上面的坐标同一空间）。 */
  screenshot(): Promise<string>;
  /** 截图 → **原始帧**（JZImage，不编码）。给"要自己处理像素"的用 ——
   *  例如 `cv.fromImage(img)` 走 OpenCV，省掉 dataURL 那条路的 PNG 编码 + 再解码两遍。
   *  要给人看 / 送模型的**图片**仍用上面那个 screenshot()；两者尺寸口径完全一致。 */
  screenshotImage(): Promise<JZImageLike>;
  /** 屏幕尺寸/缩放：把"图上的像素"换算成物理坐标的依据。后端没实现时缺省。 */
  screenInfo?(): Promise<ScreenInfo>;
  /** 导出可访问布局树（可指定目标窗口）；没有目标窗口 / 没有可访问布局返回 null。 */
  layoutTree?(opts?: LayoutTreeOptions): Promise<LayoutNode | null>;
  /** 等界面稳定（像素只当信号）；缺省退化为 wait(settle)。 */
  waitStable?(opts?: SyncOptions): Promise<boolean>;
  /** 启动 / 激活目标窗口。 */
  launch?(opts: LaunchOptions): Promise<boolean>;
  saveModel(app: IApp, path: string): Promise<void>;
  loadModel(path: string): Promise<IApp>;
  explore(opts: ExploreOptions): Promise<IApp>;
  fromPerception(tree: unknown): Promise<IApp>;
}

export let backend: ComputerUseBackend;
export function setBackend(b: ComputerUseBackend): void;
/** 最近一次 layoutTree 的来源：'semantic'(自家语义树) / 'win32'(外部子窗口枚举) / ''。 */
export function layoutSource(): string;

// ————————————————————————————————————————————————
// 语义节点接口
// ————————————————————————————————————————————————

export interface ICuNode {
  readonly id: string;
  readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  /** 布局路径（运行期寻址依据）。 */
  layout?: LayoutPath;
  /** true = 显式标出的"退化到像素"项。 */
  degraded?: boolean;
  presence?: Presence;
  parent?: ICuNode;
  readonly children: ICuNode[];

  activate(): Promise<void>;
  ensureActivated(): Promise<void>;
  /** 解析为屏幕真实坐标：layout → locator → rect。 */
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
  setFraction(f: number): Promise<void>;
}
export interface ILink extends ICuNode { readonly type: 'link'; click(btn?: MouseBtn): Promise<void>; }

export interface IApp extends ICuNode {
  readonly type: 'app';
  /** **期望指纹**（建模期记下；attach 不覆盖它）。 */
  fingerprint?: PageFingerprint;
  /** 目标窗口（`title` 片段）；空 = 用前台窗口。 */
  target?: LayoutTreeOptions;
  /** 运行期修复覆盖层（`旧路径 → 当前路径`）—— 模型只改这里。 */
  override: Record<LayoutPath, LayoutPath>;

  /** 所有节点（含自己，父先于子）。 */
  nodes(): ICuNode[];
  find(type: string, label: string): ICuNode | undefined;
  button(label: string): IButton;
  /** ⭐ 按布局路径取节点（取不到抛 `not-found`）。 */
  at(path: LayoutPath): ICuNode;
  has(path: LayoutPath): Promise<boolean>;
  /** 当前布局树（attach 后才有）。 */
  getLayout(): LayoutNode | null;
  /** 路径 → 实际去查的路径（跟随 override 链）。 */
  resolvePath(path: LayoutPath): LayoutPath;
  /** 按布局路径问当前几何（JS 侧索引）。 */
  locateLayoutPath(path: LayoutPath): Promise<Rect | null>;
  /** ⭐ 绑定布局（不涉像素）；`opts` 指定并记住目标窗口。 */
  attach(layout?: LayoutNode | null, opts?: LayoutTreeOptions): Promise<AttachResult>;
  /** 读节点状态（布局优先）。 */
  read(target: LayoutPath | ICuNode): Promise<NodeState>;
  /** 断言节点状态（不满足抛 `expect-failed`）。 */
  expect(target: LayoutPath | ICuNode, matcher: ExpectMatcher): Promise<NodeState>;
  /** 等界面稳定（像素只当信号）。 */
  sync(opts?: SyncOptions): Promise<boolean>;
  /** 启动 / 激活目标窗口。 */
  launch(opts: LaunchOptions): Promise<boolean>;
  /** 界面不匹配的本地分级（先拉一次当前布局再判）。 */
  diagnose(err: unknown,
           opts?: { attempt?: number; maxAttempts?: number; path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis>;

  save(path: string): Promise<void>;
}

// ————————————————————————————————————————————————
// 节点类
// ————————————————————————————————————————————————

/** 本包错误：带 `code`。 */
export class CuError extends Error {
  code: CuErrorCode;
  constructor(code: CuErrorCode, message: string);
}

export abstract class CuNode implements ICuNode {
  constructor(label: string, opts?: NodeOpts);
  id: string;
  abstract readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  layout?: LayoutPath;
  degraded?: boolean;
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
  /** **期望指纹**（建模期记下；attach 不覆盖它）。 */
  fingerprint?: PageFingerprint;
  /** 目标窗口（`title` 片段）。 */
  target?: LayoutTreeOptions;
  /** 运行期修复覆盖层（`旧路径 → 当前路径`）。 */
  override: Record<LayoutPath, LayoutPath>;
  constructor(name: string, opts?: NodeOpts);
  /** 所有节点（含自己，父先于子）。 */
  nodes(): CuNode[];
  find(type: string, label: string): CuNode | undefined;
  button(l: string): IButton;
  at(path: LayoutPath): CuNode;
  getLayout(): LayoutNode | null;
  resolvePath(path: LayoutPath): LayoutPath;
  locateLayoutPath(path: LayoutPath): Promise<Rect | null>;
  has(path: LayoutPath): Promise<boolean>;
  attach(layout?: LayoutNode | null, opts?: LayoutTreeOptions): Promise<AttachResult>;
  read(target: LayoutPath | ICuNode): Promise<NodeState>;
  expect(target: LayoutPath | ICuNode, matcher: ExpectMatcher): Promise<NodeState>;
  sync(opts?: SyncOptions): Promise<boolean>;
  launch(opts: LaunchOptions): Promise<boolean>;
  diagnose(err: unknown,
           opts?: { attempt?: number; maxAttempts?: number; path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis>;
  save(path: string): Promise<void>;
  static load(path: string): Promise<IApp>;
  static explore(opts: ExploreOptions): Promise<IApp>;
  static fromPerception(tree: unknown): Promise<IApp>;
  /** ⭐ 由"感知结果 + 布局树"直接装配。 */
  static build(tree: unknown, opts?: { layout?: LayoutNode | null }): IApp;
}

// ————————————————————————————————————————————————
// 底层 primitive(通用 OS 动作, 非树内控件)
// ————————————————————————————————————————————————
/** 敲一下（按下 + 抬起）；事件驱动的消费者够用，轮询按键的应用见 keyDown/keyUp。 */
export function key(name: string): Promise<void>;
/** 按住（键盘状态；按住多久由调用方决定，直到 keyUp）。 */
export function keyDown(name: string): Promise<void>;
/** 抬起（与 keyDown 成对）。 */
export function keyUp(name: string): Promise<void>;
export function type(text: string): Promise<void>;
export function wait(ms: number): Promise<void>;

// ————————————————————————————————————————————————
// 感知层(可换后端: VLM / OCR / 检测 都在 perceive.ts 这一个文件里)
// ————————————————————————————————————————————————
export function setVlmConfig(cfg: VlmOptions | null): void;
export function vlmConfig(): VlmOptions | null;
export function perceive(shot: string, opts?: { vlm?: VlmOptions }): Promise<Perception>;
export function buildPrompt(imgW: number, imgH: number): string;

// ————————————————————————————————————————————————
// 布局工具 + 本地分级
// ————————————————————————————————————————————————
export interface FlatEntry { node: LayoutNode; path: LayoutPath; depth: number; }
export function flatten(root?: LayoutNode | null): FlatEntry[];
export function nodeAt(root: LayoutNode | null | undefined, path: LayoutPath): LayoutNode | null;
export function parentPath(path: LayoutPath): string;
export function fingerprintOf(root: LayoutNode | null | undefined,
                             opts?: { max?: number; window?: string }): PageFingerprint;
export function fingerprintMatch(recorded: PageFingerprint | null | undefined,
                                live: LayoutNode | null | undefined): number;
export function samePage(recorded: PageFingerprint | null | undefined,
                         live: LayoutNode | null | undefined, threshold?: number): boolean;
export function extraTopPaths(recorded: PageFingerprint | null | undefined,
                              live: LayoutNode | null | undefined): LayoutPath[];
export function claimLayout(entry: { rect: Rect; type?: string }, flat: FlatEntry[],
                            opts?: { minCover?: number }): FlatEntry | null;
export function layoutCoverRatio(outer: Rect, inner: Rect): number;

export function diagnose(err: unknown, ctx?: DiagnoseContext): Diagnosis;
export function formatDiagnosis(d: Diagnosis): string;
export function stateMatches(state: NodeState, matcher: ExpectMatcher): boolean;
export function explainMismatch(state: NodeState, matcher: ExpectMatcher): string;

/** 升级大模型时的**证据包**（期望 + 实际 + 历史）。 */
export interface EvidencePack {
  step?: string;
  path?: LayoutPath;
  expect?: ExpectMatcher;
  attempt?: number;
  maxAttempts?: number;
  diagnosis: Diagnosis;
  fingerprint?: unknown;
  layout?: LayoutNode | null;
  history?: { retries?: number; lastModelAnswer?: string };
}
export function formatEvidence(pack: EvidencePack): string;

// ————————————————————————————————————————————————
// 运行期修复
// ————————————————————————————————————————————————
/** ⭐ 界面不匹配 → 本地分级 →（只有真错才）升级大模型 → 把修法落进 override。 */
export function escalate(app: RepairTarget, err: unknown, opts: RepairOptions): Promise<RepairResult>;
