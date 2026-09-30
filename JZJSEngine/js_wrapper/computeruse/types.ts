// computeruse 包 —— 类型契约（与实现解耦）。
//
// 坐标一律归一化 [0,1]，相对"截图 / 虚拟桌面"。
// C++ 后端(JZJSComputerUseModule)只提供底层语义; 控件树与 UI 操作全在 JS 侧。
//
// ⭐ 两条铁律（见 docs/compute-use-design.md）：
//   ① 像素只在**建模期**出现 —— 建模时产出"像素 ↔ 布局映射"；运行期只按**布局路径**寻址
//      （`Locator.kind='layout'`；像素族 ocr/image/color/yolo 只在建模期辅助与无布局兜底时用）。
//   ② 界面不匹配先**本地分级**（偶发 = 重试后不再复现），稳定复现才升级大模型。

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
 * 布局路径：从窗口往下拼出来的稳定标识（如 `/main/btnOutput`）。
 * 建模期由 `dumpSemanticTree` 的 `viewId`(= `objectName()`) 生成，**运行期寻址的唯一依据**。
 */
export type LayoutPath = string;

/**
 * 定位策略：建模期决定"这个控件靠什么被找到"。
 *
 *   layout ⭐ 运行期正路：按布局路径问控件**当前**几何（不涉像素）。
 *   ocr    基本文字按钮（OCR 找文字，最省事）
 *   image  图标按钮（模板匹配，单尺度，当前不做多尺度）
 *   color  颜色块（colorFind 按颜色）
 *   yolo   复杂图标（小检测模型，几个样本即可训练）
 *
 * ⚠ `ocr / image / color / yolo` 是**像素族**：只用于①建模期把像素认领到布局节点、
 *   ②目标没有可访问布局时的**显式兜底**（此时模型节点带 `degraded: true`）。
 *
 * region 为可选的小搜索区域（归一化）；不传则搜整屏。
 */
export type Locator =
  | { kind: 'layout'; path: LayoutPath; region?: Rect }
  | { kind: 'ocr';   text: string; region?: Rect }
  | { kind: 'image'; image: string; region?: Rect }
  | { kind: 'color'; color: string; region?: Rect; tol?: number }
  | { kind: 'yolo';  model: string; label?: string; region?: Rect };

// ————————————————————————————————————————————————
// 布局（可访问性）树 —— 建模期建映射、运行期寻址与读状态都用它
// ————————————————————————————————————————————————

/**
 * 可访问布局树节点（无障碍树形态）。
 * 自家界面 = `JZWidget::dumpSemanticTree`；外部软件 = Win32 子窗口 / UI Automation / MSAA。
 * 后端负责把屏幕坐标换算成**归一化** rect（与本包口径一致）。
 */
export interface LayoutNode {
  /** className 槽位："window" / "group" / "button" / "textbox" ... */
  role: string;
  /** viewId = `objectName()` —— 布局路径的稳定片段（没有就用 role）。 */
  id?: string;
  /** 可见文本（按钮/输入框/标签）。 */
  text?: string;
  /** contentDesc：人类可读名（objectName 优先）。 */
  name?: string;
  /** 归一化矩形（相对整屏/虚拟桌面）。 */
  rect?: Rect;
  visible?: boolean;
  enabled?: boolean;
  clickable?: boolean;
  checked?: boolean;
  /** RangeInfo（slider/progress/spinbox）。 */
  range?: { value: number; min: number; max: number };
  items?: string[];
  actions?: string[];
  children?: LayoutNode[];
}

/**
 * 页面指纹：窗口标识 + 关键节点路径集合。
 * 用来判"还在不在同一页" —— 这是"偶发 vs 真错"里 page-miss / path-drift 的分界依据。
 */
export interface PageFingerprint {
  window?: string;
  paths: LayoutPath[];
}

/** `attach()` 的结果：模型 ↔ 布局的绑定情况。 */
export interface AttachResult {
  window?: string;
  /** **当前**页面的指纹（attach 时算的；模型里记的是"期望指纹"，两者不是一回事）。 */
  fingerprint: PageFingerprint;
  /** 期望指纹与当前页面的吻合度（[0,1]；模型里没记指纹时为 1）。 */
  pageMatch: number;
  /** 模型里的节点总数。 */
  total: number;
  /** 在布局里认领成功的节点数。 */
  bound: number;
  /** 仍要靠像素的节点（退化项）。 */
  degraded: LayoutPath[];
  /** 布局里找不到的模型节点。 */
  missing: LayoutPath[];
}

/** 节点的当前状态（`read()` 的返回）—— 布局能答的就问布局，答不了才退像素。 */
export interface NodeState {
  exists: boolean;
  /** 状态来源：'layout' = 问的布局；'rect' = 只拿到几何（布局缺失时的降级）。 */
  from: 'layout' | 'rect';
  text?: string;
  checked?: boolean;
  enabled?: boolean;
  clickable?: boolean;
  range?: { value: number; min: number; max: number };
  rect?: Rect;
}

/** `expect()` 的期望条件：字符串/正则比文本；数字比 range.value；对象可逐项指定。 */
export type ExpectMatcher =
  | string
  | RegExp
  | number
  | boolean
  | {
      text?: string | RegExp;
      checked?: boolean;
      enabled?: boolean;
      exists?: boolean;
      range?: { value?: number; min?: number; max?: number };
    };

/**
 * `layoutTree()` 参数：**指定目标窗口**。
 * `title` 为空/不给 ⇒ 用"前台窗口"；给子串 ⇒ 按标题片段找。
 * ⚠ 自动化脚本常常跑在终端里（前台窗口是终端本身）⇒ 靠前台猜必错，能指定就指定。
 */
export interface LayoutTreeOptions {
  title?: string;
}

/** `sync()` 参数：等界面稳定（像素在这里只当**信号**，不参与寻址）。 */
export interface SyncOptions {
  /** 连续多少毫秒没有变化就算稳定（默认 300）。 */
  settle?: number;
  /** 最长等待（默认 10000）。 */
  timeoutMs?: number;
}

/** `launch()` 参数：启动 / 激活目标窗口（外部软件的入口）。 */
export interface LaunchOptions {
  /** 可执行文件路径（不给则只做"激活已存在的窗口"）。 */
  exe?: string;
  args?: string[];
  cwd?: string;
  /** 目标窗口标题（字符串=包含匹配；正则=test）。 */
  title?: string | RegExp;
  timeoutMs?: number;
}

// ————————————————————————————————————————————————
// 界面不匹配的**本地分级**（先本地判，稳定复现才升级大模型）
// ————————————————————————————————————————————————

/** 失败性质。 */
export type FailureKind =
  | 'none'          // 不是失败
  | 'transient'     // A 偶发（时序）：等一等 / 重试就好
  | 'popup'         // C 多出弹窗/对话框：本地关掉再重试
  | 'precondition'  // B 找到了但不可用（enabled/checked/range 不满足前置）
  | 'page-miss'     // D 稳定复现缺失且页面指纹变了（走错流程 / 换了界面）
  | 'path-drift'    // E 同页但节点路径变了（版本 / 布局漂移）
  | 'process-gone'  // F 窗口 / 进程消失
  | 'no-layout';    // 没有可访问布局（退化到像素，或该目标本来就没有布局）

/** 本地处置建议（模型不在场时也能执行）。 */
export type FailureAction =
  | 'retry' | 'wait' | 'dismiss-popup' | 'add-precondition'
  | 'ask-model' | 'restart' | 'abort';

/** `diagnose()` 的输入。 */
export interface DiagnoseContext {
  /** 第几次尝试（1-based）。 */
  attempt?: number;
  /** 一共允许多少次。 */
  maxAttempts?: number;
  /** 模型里记的期望指纹。 */
  fingerprint?: PageFingerprint | null;
  /** 当前布局树（拿不到就是没有布局通道）。 */
  layout?: LayoutNode | null;
  /** 出问题的路径。 */
  path?: LayoutPath;
  /** 该步骤的期望条件。 */
  expect?: ExpectMatcher;
}

/** 分级结论。 */
export interface Diagnosis {
  kind: FailureKind;
  /** 是否"偶发"（可以自动重试，不需要模型）。 */
  transient: boolean;
  action: FailureAction;
  reason: string;
  evidence: {
    path?: LayoutPath;
    /** 页面指纹对不上（跨页）。 */
    fingerprintChanged?: boolean;
    /** 布局里多出来的顶层节点（疑似弹窗）。 */
    extraPaths?: string[];
    /** 该路径在布局里不存在。 */
    missingPath?: string;
    /** 节点在布局里但不可用（enabled/checked/range 不满足）。 */
    foundButUnusable?: string;
  };
}

/** 本包抛出的错误码（`diagnose()` 靠它做本地分类）。 */
export type CuErrorCode =
  | 'not-found'      // 控件不存在
  | 'no-layout'      // 没有布局通道
  | 'expect-failed'  // 断言不满足
  | 'window-gone'    // 窗口/进程消失
  | 'send-failed'    // 动作注入失败
  | 'unsupported';   // 后端没实现该原语

// ————————————————————————————————————————————————
// 运行期修复：界面不匹配 → （只有真错才）升级大模型 → 落 override
// ————————————————————————————————————————————————

/**
 * 升级所需的最小 App 契约（结构化类型：repair.ts 不 import App 类，避免循环引用）。
 * `App` 天然满足它。
 */
export interface RepairTarget {
  diagnose(err: unknown, opts?: { attempt?: number; maxAttempts?: number;
                                 path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis>;
  /** 当前布局树（升 级证据包的主体）。 */
  getLayout(): LayoutNode | null;
  /** 运行期修复覆盖层（`旧路径 → 当前路径`）—— 模型**只**改这里。 */
  override: Record<LayoutPath, LayoutPath>;
  /** 建模期记下的期望指纹（判"还在不在同一页"）。 */
  fingerprint?: PageFingerprint;
}

/** 修复用模型端点（OpenAI 兼容 chat/completions；与感知同一个后端也能用）。 */
export interface RepairModelOptions {
  /** 完整端点，如 http://host:8080/v1/chat/completions */
  url: string;
  model?: string;
  apiKey?: string;
  timeoutMs?: number;
  maxTokens?: number;
  /** 整段替换内置提示词。 */
  prompt?: string;
  extraBody?: Record<string, unknown>;
}

/** `escalate()` 的参数。 */
export interface RepairOptions extends RepairModelOptions {
  /** 出问题的路径（模型要修的就是它）。 */
  path: LayoutPath;
  /** 该步骤的期望条件（进证据包）。 */
  expect?: ExpectMatcher;
  attempt?: number;
  maxAttempts?: number;
  step?: string;
  history?: { retries?: number; lastModelAnswer?: string };
}

/** 模型给的结论（**只能从当前布局树里挑路径**，编造的一律不采纳）。 */
export interface RepairAnswer {
  kind?: string;
  action?: string;
  newPath?: string;
  reason?: string;
}

/** `escalate()` 的结果。 */
export interface RepairResult {
  diagnosis: Diagnosis;
  /** true = 本地就处理掉了（偶发/弹窗/前置），**没有叫模型**。 */
  localOnly: boolean;
  answer: RepairAnswer;
  /** newPath 是否已写进 override。 */
  applied: boolean;
  override?: { from: LayoutPath; to: LayoutPath };
  /** 实际发给模型的证据包（留档/复现用）。 */
  evidence?: string;
  raw?: string;
}

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

/** 人审/标注钩子：拿到感知结果 → 改（删错检/补漏检/改类型文字/调层级）→ 还回来。 */
export type PerceptionReview = (p: Perception) => Perception | Promise<Perception>;

/** 建模（探索）阶段配置。 */
export interface ExploreOptions {
  name: string;
  screenshot?: { maxDim?: number };
  anchor?: { pad?: number };
  /** 感知后端；不传则用 setVlmConfig() 设的默认值，都没有就抛错。 */
  vlm?: VlmOptions;
  /** ③ 人审/标注（可选）。不传 = 行为完全不变。 */
  review?: PerceptionReview;
  /** 布局树（建模期用它把感知元素**认领**到布局节点 ⇒ 生成像素↔布局映射）。 */
  layout?: LayoutNode | null;
}

/** 模型序列化快照（固化到磁盘的格式）。 */
export interface ModelSnapshot {
  name: string;
  nodes: unknown[];
}

export interface NodeOpts { locator?: Locator; rect?: Rect; layout?: LayoutPath; }

// ————————————————————————————————————————————————
// 底层动作后端：由运行期 JZJSComputerUseModule 注入 __jz.computeruse。
// ————————————————————————————————————————————————
export interface ComputerUseBackend {
  /**
   * 按定位策略查找，返回命中矩形；找不到返回 null。
   * ⭐ 运行期应**只出现** `kind:'layout'` —— 此时它是"按布局路径问控件当前几何"，不涉像素；
   *   像素族（ocr/image/color/yolo）只用于建模期与退化兜底。单尺度。
   */
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
  /** 虚拟桌面截图 → dataURL（建模期为主；运行期作同步信号/证据）。 */
  screenshot(): Promise<string>;

  /** 导出可访问布局树（可指定目标窗口）；没有目标窗口 / 没有可访问布局时返回 null。 */
  layoutTree?(opts?: LayoutTreeOptions): Promise<LayoutNode | null>;
  /** 等界面稳定：像素在实现里只当**信号**（不收敛返回 false）。缺省实现退化为 wait(settle)。 */
  waitStable?(opts?: SyncOptions): Promise<boolean>;
  /** 启动 / 激活目标窗口（外部软件入口）。 */
  launch?(opts: LaunchOptions): Promise<boolean>;

  // —— 模型固化 / 装配（JS 侧实现，序列化本树的 JSON）——
  saveModel(app: IApp, path: string): Promise<void>;
  loadModel(path: string): Promise<IApp>;
  /** ⭐ 建模入口：截图 → 感知 → (人审) → 认领布局 → 语义树。 */
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
  /** ⭐ 布局路径（运行期寻址依据）。没有 = 该节点只能靠像素族 locator。 */
  layout?: LayoutPath;
  /** true = 无可访问布局，显式标出的退化项。 */
  degraded?: boolean;
  presence?: Presence;
  parent?: ICuNode;
  readonly children: ICuNode[];

  activate(): Promise<void>;
  ensureActivated(): Promise<void>;
  /** 解析为屏幕真实坐标：**layout → locator → rect**（不涉像素的优先）。 */
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
// 这是附带能力，不需要 Browser/WebElement 这类独立控件。有可访问 DOM/可访问性树时优先 layout。
export interface IApp extends ICuNode {
  readonly type: 'app';
  /** 页面指纹（建模期记下，运行期判"还在不在同一页"）。 */
  fingerprint?: PageFingerprint;
  /** 目标窗口（`title` 片段）；空 = 用前台窗口。 */
  target?: LayoutTreeOptions;
  /** 运行期修复覆盖层（`旧路径 → 当前路径`）—— 模型只改这里。 */
  override: Record<LayoutPath, LayoutPath>;

  /** 所有节点（含自己，父先于子）。 */
  nodes(): ICuNode[];
  find(type: string, label: string): ICuNode | undefined;
  button(label: string): IButton;
  /** ⭐ 按**布局路径**取节点（取不到抛 `not-found`）。 */
  at(path: LayoutPath): ICuNode;
  has(path: LayoutPath): Promise<boolean>;

  /**
   * ⭐ 绑定布局（**不涉像素**）：拉一次布局树，把模型节点认领到布局节点上。
   * 不传 `layout` 则由后端拉（`backend.layoutTree(this.target)`）；拿不到 ⇒ 全部节点标 `degraded`。
   * `opts` 用来**指定目标窗口**，并**记住**（后续 read/diagnose 刷新布局也用它）。
   */
  attach(layout?: LayoutNode | null, opts?: LayoutTreeOptions): Promise<AttachResult>;
  /** 读节点当前状态（布局优先，布局读不到才退到几何）。 */
  read(target: LayoutPath | ICuNode): Promise<NodeState>;
  /** 断言节点当前状态；不满足抛 `expect-failed`（脚本级自检）。 */
  expect(target: LayoutPath | ICuNode, matcher: ExpectMatcher): Promise<NodeState>;
  /** 等界面稳定（像素在这里只当**信号**）。 */
  sync(opts?: SyncOptions): Promise<boolean>;
  /** 启动 / 激活目标窗口。 */
  launch(opts: LaunchOptions): Promise<boolean>;
  /** 界面不匹配的本地分级（偶发 vs 真错）；会先拉一次**当前**布局再判。 */
  diagnose(err: unknown, opts?: { attempt?: number; maxAttempts?: number; path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis>;

  save(path: string): Promise<void>;
}
