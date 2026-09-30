/// <reference path="../types/globals.d.ts" />
// computeruse 包 —— 运行期实现（控件树 + UI 操作在 JS；底层语义走 __jz.computeruse）。
//
// 设计：
//   - 控件树(App/Tab/Button/...)与所有 UI 操作都在 JS 侧完成。
//   - 真正的"操作系统"动作(点击/按键/截图/查找) 100% 由 C++ JZJSComputerUseModule 提供，
//     这里只通过 __jz.computeruse.* 调用, 不重复造轮子, 也不在 C++ 侧构建控件树。
//   - ocr / yolo 两种定位策略的"后端"暂未实现; C++ 侧 match 对它们返回未找到, 待接入。

import type {
  Rect, Point, MouseBtn, Presence, Locator, ExploreOptions,
  ComputerUseBackend, ICuNode, IButton, ITab, IMenu, IToolbar, IGroup, IWindow,
  ICanvas, ILabel, IEdit, IComboBox, ICheckBox, ISlider, ILink, IApp,
  Perception, PerceptionElement,
  LayoutPath, LayoutNode, PageFingerprint, AttachResult, NodeState, ExpectMatcher,
  SyncOptions, LaunchOptions, Diagnosis, DiagnoseContext, CuErrorCode,
} from './types';
import { perceive } from './perceive';
import { claimLayout, fingerprintMatch, fingerprintOf, flatten } from './layout';
import { diagnose, explainMismatch, stateMatches } from './diagnose';

declare const __jz: any;
// 引擎注入的全局 console（本包不依赖 DOM/Node 类型库）
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

/** 本包的错误：带 `code`（`diagnose()` 靠它做本地分级，不靠错误消息文本）。 */
export class CuError extends Error {
  code: CuErrorCode;
  constructor(code: CuErrorCode, message: string) {
    super(message);
    this.name = 'CuError';
    this.code = code;
  }
}

function cu(): any {
  if (!__jz || !__jz.computeruse)
    throw new Error('[CU] 后端未加载：__jz.computeruse 不存在（需在 C++ 侧注册 JZJSComputerUseModule 并接入引擎）');
  return __jz.computeruse;
}
function cuCore(): any {
  if (!__jz || !__jz.core) throw new Error('[CU] __jz.core 未加载');
  return __jz.core;
}

// —— 默认后端：对接 C++ JZJSComputerUseModule ——
export let backend: ComputerUseBackend = {
  match: async (locator) => {
    const r = cu().match(JSON.stringify(locator));
    return r ? { x: r.x, y: r.y, w: r.w, h: r.h } : null;
  },
  click: async (rect, btn) => cu().click(rect.x + rect.w / 2, rect.y + rect.h / 2, btn ?? 'left'),
  drag: async (from, to) => cu().drag(from.x, from.y, to.x, to.y),
  key: async (name) => cu().key(name),
  type: async (text) => cu().type(text),
  wait: async (ms) => cu().wait(ms),
  screenshot: async () => cu().screenshot(),
  // —— 布局通道（运行期寻址的**正路**，见 docs/compute-use-design.md §4.1）——
  // C++ 侧还没实现 `layoutTree` 时**返回 null**（而不是抛错）："没有可访问布局"是个可判定的
  // 状态 —— 调用方会把节点标成 degraded（显式退化到像素），绝不让它悄悄变成默认行为。
  layoutTree: async (): Promise<LayoutNode | null> => {
    const fn = __jz && __jz.computeruse ? __jz.computeruse.layoutTree : undefined;
    if (!fn) return null;
    const r = fn();
    if (!r) return null;
    return (typeof r === 'string' ? JSON.parse(r) : r) as LayoutNode;
  },
  // 等界面稳定：像素在实现里只当**信号**（帧间变化是否收敛），不参与寻址。
  // 后端没实现就退化为 wait(settle) —— 语义弱一点，但行为不变。
  waitStable: async (opts?: SyncOptions): Promise<boolean> => {
    const cuu = __jz ? __jz.computeruse : null;
    if (cuu && cuu.waitStable) return !!cuu.waitStable(JSON.stringify(opts || {}));
    await cu().wait((opts && opts.settle) || 300);
    return true;
  },
  // 启动 / 激活目标窗口（外部软件入口）。C++ 侧待补 ⇒ 明确报"未实现"，不假装成功。
  launch: async (opts: LaunchOptions): Promise<boolean> => {
    const cuu = __jz ? __jz.computeruse : null;
    if (!cuu || !cuu.launch)
      throw new CuError('unsupported', '[CU] 后端没有 launch（启动/激活目标窗口）—— C++ 侧待补');
    return !!cuu.launch(JSON.stringify(opts));
  },
  // 模型固化在 JS 侧(序列化本树 JSON), 经 __jz.core 落盘。
  // ⚠ `__jz.core.writeFile(path, data, append)` 是**三参**原生绑定 ⇒ 少传 append 会报
  //   "参数错误:需要3个参数"(只有真跑过保存路径才会暴露 —— 离线后端不走它)。
  saveModel: async (app, path) => {
    cuCore().writeFile(path, JSON.stringify(serialize(app), null, 2), false);
  },
  loadModel: async (path) => {
    const data = cuCore().readFile(path);
    const text = typeof data === 'string' ? data : new TextDecoder().decode(data);
    return deserialize(JSON.parse(text)) as unknown as IApp;
  },
  // 建模（探索）= ① 截一张**客户区**像素图 ② 感知后端(perceive.ts)给出元素清单
  //              ③ 人审/标注(可选 review) ④ **认领布局** ⇒ 生成"像素 ↔ 布局映射" ⑤ 装配成树。
  // ⭐ 像素只出现在 ①②；从 ④ 起全部走布局 —— 这就是"建模期 / 运行期"的分界。
  explore: async (opts: ExploreOptions): Promise<IApp> => {
    const shot = await backend.screenshot();
    let p: Perception | null = await perceive(shot, opts ? { vlm: opts.vlm } : undefined);
    if (opts && opts.review && p) p = await opts.review(p);          // 人审/标注（默认不启用）
    let layout: LayoutNode | null = null;
    if (opts && opts.layout !== undefined) layout = opts.layout || null;
    else layout = backend.layoutTree ? await backend.layoutTree() : null;
    const app = buildTree(p, layout) as App;
    if (layout) await app.attach(layout);                            // 建索引 + 标退化项
    return app;
  },
  fromPerception: async (tree: unknown): Promise<IApp> => buildTree(tree as Perception),
};

export function setBackend(b: ComputerUseBackend): void { backend = b; }

let _seq = 0;
function genId(): string { return 'cu' + (++_seq); }

interface NodeOpts { locator?: Locator; rect?: Rect; layout?: LayoutPath; degraded?: boolean; }

// ————————————————————————————————————————————————
// 节点基类：所有控件共用"定位→点击"语义
// ————————————————————————————————————————————————
export abstract class CuNode implements ICuNode {
  id: string;
  abstract readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  /** ⭐ 布局路径（运行期寻址的**唯一依据**；建模期的"像素↔布局映射"就落在这里）。 */
  layout?: LayoutPath;
  /** true = 没有可访问布局，**显式标出**的"退化到像素"项（见 docs §4.1 推论 3）。 */
  degraded?: boolean;
  presence?: Presence;
  parent?: CuNode;
  children: CuNode[] = [];

  constructor(label: string, opts?: NodeOpts) {
    this.label = label;
    this.id = genId();
    if (opts) {
      this.locator = opts.locator; this.rect = opts.rect;
      this.layout = opts.layout; this.degraded = opts.degraded;
    }
  }

  protected add<T extends CuNode>(n: T): T { n.parent = this; this.children.push(n); return n; }
  append(child: CuNode): CuNode { child.parent = this; this.children.push(child); return child; }

  async activate(): Promise<void> {}
  async ensureActivated(): Promise<void> {
    if (this.parent) await this.parent.ensureActivated();
    await this.activate();
  }
  // 解析为屏幕真实坐标：**layout（布局路径）→ locator（像素族）→ rect**。
  // ⚠ 布局失败时**不静默滑到像素**：只有显式标了 `degraded` 的节点才允许像素兜底 ——
  //    否则"能看见布局就别猜"这条铁律就形同虚设（见 docs §4.1 推论 3）。
  async locate(): Promise<Rect | null> {
    if (this.layout) {
      const hit = await backend.match({ kind: 'layout', path: this.layout });
      if (hit) return hit;
      if (!this.degraded) return null;
    }
    if (this.locator) return backend.match(this.locator);
    if (this.rect) return this.rect;
    return null;
  }
  async exists(): Promise<boolean> { return (await this.locate()) !== null; }

  // 通用点击：先判存在，再注入；失败抛异常。Button/Link/WebElement 直接复用。
  async click(btn: MouseBtn = 'left'): Promise<void> {
    const hit = await this.locate();
    if (!hit) throw new CuError('not-found',
      `[CU] 控件 "${this.label}" 未找到（布局路径 ${this.layout || '(无布局)'}${this.degraded ? '，退化到像素' : ''}）`);
    const ok = await backend.click(hit, btn);
    if (!ok) throw new CuError('send-failed', `[CU] 点击失败: "${this.label}"`);
  }

  // —— 子节点构造器（含常见控件，参考 JZWidget 词汇）——
  addTab(l: string, o?: Locator): ITab { return this.add(new Tab(l, o)); }
  addMenu(l: string, o?: Locator): IMenu { return this.add(new Menu(l, o)); }
  addToolbar(l: string, o?: Locator): IToolbar { return this.add(new Toolbar(l, o)); }
  addGroup(l: string, o?: Locator): IGroup { return this.add(new Group(l, o)); }
  addWindow(l: string, o?: Locator): IWindow { return this.add(new Window(l, o)); }
  addCanvas(l: string, o?: Locator): ICanvas { return this.add(new Canvas(l, o)); }
  addButton(l: string, loc: Locator): IButton { return this.add(new Button(l, loc)); }
  addEdit(l: string, loc: Locator): IEdit { return this.add(new Edit(l, loc)); }
  addComboBox(l: string, loc: Locator): IComboBox { return this.add(new ComboBox(l, loc)); }
  addCheckBox(l: string, loc: Locator): ICheckBox { return this.add(new CheckBox(l, loc)); }
  addLabel(l: string, o?: Locator): ILabel { return this.add(new Label(l, o)); }
  addSlider(l: string, loc: Locator): ISlider { return this.add(new Slider(l, loc)); }
  addLink(l: string, loc: Locator): ILink { return this.add(new Link(l, loc)); }
}

// —— 点击型控件（复用基类 click）——
export class Button extends CuNode implements IButton {
  readonly type = 'button';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
}
export class Link extends CuNode implements ILink {
  readonly type = 'link';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
}

// —— 激活型容器：Tab / Menu ——
export class Tab extends CuNode implements ITab {
  readonly type = 'tab';
  constructor(label: string, locator?: Locator) { super(label, locator ? { locator } : undefined); }
  async activate(): Promise<void> {
    const r = await this.locate();
    if (!r) throw new Error(`[CU] 选项卡未找到: "${this.label}"`);
    const ok = await backend.click(r);
    if (!ok) throw new Error(`[CU] 激活选项卡失败: "${this.label}"`);
    await backend.wait(150);
  }
}
export class Menu extends CuNode implements IMenu {
  readonly type = 'menu';
  constructor(label: string, locator?: Locator) { super(label, locator ? { locator } : undefined); }
  async activate(): Promise<void> {
    const r = await this.locate();
    if (!r) throw new Error(`[CU] 菜单未找到: "${this.label}"`);
    const ok = await backend.click(r);
    if (!ok) throw new Error(`[CU] 展开菜单失败: "${this.label}"`);
    await backend.wait(150);
  }
}

// —— 纯容器 / 占位 ——
export class Toolbar extends CuNode implements IToolbar { readonly type = 'toolbar'; constructor(l: string, o?: Locator){ super(l, o?{locator:o}:undefined); } }
export class Group extends CuNode implements IGroup { readonly type = 'group'; constructor(l: string, o?: Locator){ super(l, o?{locator:o}:undefined); } }
export class Window extends CuNode implements IWindow { readonly type = 'window'; constructor(l: string, o?: Locator){ super(l, o?{locator:o}:undefined); } }
export class Label extends CuNode implements ILabel { readonly type = 'label'; constructor(l: string, o?: Locator){ super(l, o?{locator:o}:undefined); } }

// —— 画布（拖拽画形状）——
export class Canvas extends CuNode implements ICanvas {
  readonly type = 'canvas';
  constructor(l: string, o?: Locator){ super(l, o?{locator:o}:undefined); }
  async drag(from: Point, to: Point): Promise<void> {
    const ok = await backend.drag(from, to);
    if (!ok) throw new Error(`[CU] 画布拖拽失败: "${this.label}"`);
  }
}

// —— 编辑框 ——
export class Edit extends CuNode implements IEdit {
  readonly type = 'edit';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
  async setText(text: string): Promise<void> {
    const r = await this.locate(); if (!r) throw new Error(`[CU] 编辑框未找到: "${this.label}"`);
    await backend.click(r); await backend.type(text);
  }
  async clear(): Promise<void> {
    const r = await this.locate(); if (!r) throw new Error(`[CU] 编辑框未找到: "${this.label}"`);
    await backend.click(r); await backend.key('Ctrl+A'); await backend.key('Delete');
  }
}

// —— 下拉框（点击展开 → 输入 → 回车）——
export class ComboBox extends CuNode implements IComboBox {
  readonly type = 'combobox';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
  async select(label: string): Promise<void> {
    const r = await this.locate(); if (!r) throw new Error(`[CU] 下拉框未找到: "${this.label}"`);
    await backend.click(r); await backend.wait(150); await backend.type(label); await backend.key('Return');
  }
}

// —— 复选框（状态判定需 OCR/colorFind，后续补；当前默认点击切换）——
export class CheckBox extends CuNode implements ICheckBox {
  readonly type = 'checkbox';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
  async toggle(): Promise<void> { await this.click(); }
  async check(): Promise<void> { await this.click(); }
  async uncheck(): Promise<void> { await this.click(); }
}

// —— 滑块（假设水平；垂直后续补）——
export class Slider extends CuNode implements ISlider {
  readonly type = 'slider';
  constructor(label: string, locator: Locator) { super(label, { locator }); }
  async setFraction(f: number): Promise<void> {
    const r = await this.locate(); if (!r) throw new Error(`[CU] 滑块未找到: "${this.label}"`);
    const cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const tx = r.x + r.w * Math.min(1, Math.max(0, f)), ty = cy;
    await backend.drag({ x: cx, y: cy }, { x: tx, y: ty });
  }
}

// ————————————————————————————————————————————————
// App：树根
// ————————————————————————————————————————————————
export class App extends CuNode implements IApp {
  readonly type = 'app';
  /**
   * **期望指纹**（建模期记下；运行期判"还在不在同一页" —— 偶发 vs 真错的分界依据）。
   * ⚠ `attach` **不会**覆盖它（模型运行期只读）：否则"页面变了"永远判不出来。
   *   只有模型里本来没有指纹时，才用首次 attach 的 live 指纹补一个基线。
   */
  fingerprint?: PageFingerprint;
  // 最近一次 attach 的产物（诊断的证据包用）
  private m_layout: LayoutNode | null = null;
  private m_live: PageFingerprint | undefined = undefined;
  private m_index: Map<LayoutPath, LayoutNode> = new Map();

  constructor(name: string, opts?: NodeOpts) { super(name, opts); }

  /** 所有节点（含自己，父先于子）。 */
  nodes(): CuNode[] {
    const q: CuNode[] = [this];
    for (let i = 0; i < q.length; i++) {
      const kids = q[i].children;
      for (let k = 0; k < kids.length; k++) q.push(kids[k] as CuNode);
    }
    return q;
  }

  find(type: string, label: string): CuNode | undefined {
    const all = this.nodes();
    for (const n of all) if (n.type === type && n.label === label) return n;
    return undefined;
  }
  button(l: string): IButton {
    const n = this.find('button', l);
    if (!n) throw new CuError('not-found', `[CU] 模型中不存在按钮: "${l}"`);
    return n as unknown as IButton;
  }

  /** ⭐ 按**布局路径**取模型节点（取不到抛 not-found，并列出可用的路径）。 */
  at(path: LayoutPath): CuNode {
    const all = this.nodes();
    for (const n of all) if (n.layout === path) return n;
    const near = all.filter((n) => n.layout).map((n) => String(n.layout)).slice(0, 8).join(', ');
    throw new CuError('not-found',
      `[CU] 模型里没有布局路径 "${path}"；可用: ${near || '(无 —— 该模型没有布局映射)'}`);
  }

  /** 当前布局树（attach 后才有；给诊断的证据包用）。 */
  getLayout(): LayoutNode | null { return this.m_layout; }

  async has(path: LayoutPath): Promise<boolean> {
    let n: CuNode;
    try { n = this.at(path); } catch (e) { return false; }
    return (await n.locate()) !== null;
  }

  /**
   * ⭐ 绑定布局（**不涉像素**）：拉一次布局树 → 建"路径 → 布局节点"索引 → 把模型节点认领上去。
   * 不传 `layout` 就由后端拉（`backend.layoutTree()`）；拿不到 ⇒ 全部标 `degraded`（显式退化）。
   */
  async attach(layout?: LayoutNode | null): Promise<AttachResult> {
    let tree: LayoutNode | null;
    if (layout !== undefined) { tree = layout || null; this.setLayout(tree); }
    else { tree = await this.pullLayout(); }     // 不传就由后端拉当前布局

    const all = this.nodes();
    const degraded: LayoutPath[] = [];
    const missing: LayoutPath[] = [];
    let bound = 0;
    for (const n of all) {
      if (n.layout && this.m_index.has(n.layout)) { n.degraded = false; bound++; continue; }
      if (n.layout) missing.push(n.layout);
      n.degraded = true;                       // 没法按布局寻址 ⇒ 显式退化（不是静默兜底）
      degraded.push(n.layout || ('#' + n.id + ' ' + (n.label || n.type)));
    }
    const live = fingerprintOf(tree);
    this.m_live = tree ? live : undefined;
    if (tree && !this.fingerprint) this.fingerprint = live;      // 只在模型没记指纹时补基线
    const pageMatch = fingerprintMatch(this.fingerprint, tree);
    if (degraded.length)
      console.warn('[CU] attach: ' + degraded.length + ' 个节点没有布局可寻址(退化到像素): '
        + degraded.slice(0, 5).join(', '));
    return { window: live.window, fingerprint: live, pageMatch, total: all.length, bound, degraded, missing };
  }

  /** 读节点当前状态（**布局优先**：能问布局就不读像素）。 */
  async read(target: LayoutPath | ICuNode): Promise<NodeState> {
    const n = this.resolve(target);
    if (backend.layoutTree) await this.pullLayout();   // 读状态 = 问一次**当前**布局（不啃快照）
    const live = n.layout ? this.m_index.get(n.layout) : undefined;
    if (live)
      return { exists: true, from: 'layout', text: live.text, checked: live.checked,
               enabled: live.enabled, clickable: live.clickable, range: live.range, rect: live.rect };
    const rect = await n.locate();
    return rect ? { exists: true, from: 'rect', rect } : { exists: false, from: 'rect' };
  }

  /** 断言节点当前状态；不满足抛 `expect-failed`（脚本级自检，对齐 verify.py 的硬门思路）。 */
  async expect(target: LayoutPath | ICuNode, matcher: ExpectMatcher): Promise<NodeState> {
    const n = this.resolve(target);
    const st = await this.read(n);
    if (stateMatches(st, matcher)) return st;
    throw new CuError('expect-failed',
      `[CU] 断言失败 @${n.layout || n.label}: ` + explainMismatch(st, matcher));
  }

  /** 等界面稳定：有后端信号就用它，否则退化为 wait(settle)。像素在这里只当**信号**。 */
  async sync(opts?: SyncOptions): Promise<boolean> {
    const o: SyncOptions = { settle: 300, timeoutMs: 10000, ...(opts || {}) };
    if (backend.waitStable) return backend.waitStable(o);
    await backend.wait(o.settle || 300);
    return true;
  }

  /** 启动 / 激活目标窗口（外部软件入口；后端没实现就抛 `unsupported`）。 */
  async launch(opts: LaunchOptions): Promise<boolean> {
    if (!backend.launch) throw new CuError('unsupported', '[CU] 后端没有 launch');
    return backend.launch(opts);
  }

  /**
   * 界面不匹配的**本地分级**：偶发 vs 真错（真错才该升级大模型，见 docs §1.3）。
   * ⚠ 先拉一次当前布局 —— 诊断必须看**现在**的界面，不能看 attach 时的快照。
   */
  async diagnose(err: unknown, opts?: { attempt?: number; maxAttempts?: number;
                                        path?: LayoutPath; expect?: ExpectMatcher }): Promise<Diagnosis> {
    if (backend.layoutTree) await this.pullLayout();
    const ctx: DiagnoseContext = {
      fingerprint: this.fingerprint || null,
      layout: this.m_layout,
      attempt: opts ? opts.attempt : undefined,
      maxAttempts: opts ? opts.maxAttempts : undefined,
      path: opts ? opts.path : undefined,
      expect: opts ? opts.expect : undefined,
    };
    return diagnose(err, ctx);
  }

  /** 记下布局树并重建"路径 → 布局节点"索引。 */
  private setLayout(tree: LayoutNode | null): void {
    this.m_layout = tree;
    this.m_index = new Map();
    if (tree) for (const e of flatten(tree)) this.m_index.set(e.path, e.node);
  }

  /** 拉一次**当前**布局树（读状态 / 诊断前刷新）。⚠ 不覆盖模型里的**期望指纹**。 */
  private async pullLayout(): Promise<LayoutNode | null> {
    if (!backend.layoutTree) { this.setLayout(null); return null; }
    const tree = await backend.layoutTree();
    this.setLayout(tree || null);
    return this.m_layout;
  }

  private resolve(target: LayoutPath | ICuNode): CuNode {
    return typeof target === 'string' ? this.at(target) : (target as CuNode);
  }

  async save(path: string): Promise<void> { return backend.saveModel(this, path); }

  static async load(path: string): Promise<IApp> { return backend.loadModel(path); }
  /** ⭐ 建模入口：截图 → 感知 → (人审) → 认领布局 → 语义树。 */
  static async explore(opts: ExploreOptions): Promise<IApp> { return backend.explore(opts); }
  static async fromPerception(tree: unknown): Promise<IApp> { return backend.fromPerception(tree); }
  /** ⭐ 由"感知结果 + 布局树"直接装配 —— 即"把像素认领到布局节点"那一步。 */
  static build(tree: unknown, opts?: { layout?: LayoutNode | null }): IApp {
    return buildTree(tree as Perception, opts ? opts.layout : undefined);
  }
}

// ————————————————————————————————————————————————
// 底层 primitive（通用 OS 动作，非树内控件）
// ————————————————————————————————————————————————
export async function key(name: string): Promise<void> { return backend.key(name); }
export async function type(text: string): Promise<void> { return backend.type(text); }
export async function wait(ms: number): Promise<void> { return backend.wait(ms); }

// ————————————————————————————————————————————————
// 感知结果 → 控件树（explore / fromPerception 的共用装配逻辑）
// ————————————————————————————————————————————————

// 只有这些类型可以"装下别人"(层级推断用)。判分侧的类型族见 verify.py 的 TYPE_FAMILY。
const CONTAINER_TYPES: Record<string, boolean> = {
  window: true, group: true, tab: true, menu: true, toolbar: true,
  panel: true, frame: true, list: true, table: true, tree: true,
};

/** inner 被 outer 覆盖的面积占比（矩形包含率，[0,1]）。 */
function coverRatio(outer: Rect, inner: Rect): number {
  const x0 = Math.max(outer.x, inner.x), y0 = Math.max(outer.y, inner.y);
  const x1 = Math.min(outer.x + outer.w, inner.x + inner.w);
  const y1 = Math.min(outer.y + outer.h, inner.y + inner.h);
  const iw = x1 - x0, ih = y1 - y0;
  const a = inner.w * inner.h;
  if (iw <= 0 || ih <= 0 || a <= 0) return 0;
  return (iw * ih) / a;
}

/**
 * 感知元素清单 → 控件树。
 *
 * 层级是**推断**出来的(感知只给一张平铺的框表)：面积从大到小建节点，小的挂进"能装下它的最小容器"，
 * 找不到容器就挂根窗口。根窗口自身的文本留空 —— 截图只含**客户区**，窗口标题在标题栏(非客户区)里
 * 看不见，所以按契约"看不到文字就留空串"，不给根节点编造文本。
 */
function buildTree(p: Perception | null, layout?: LayoutNode | null): IApp {
  const root = new App('');                       // ⚠ 见上：根节点不带文本（真值同样约定为空串）
  const els: PerceptionElement[] = (p && p.elements) ? p.elements : [];
  const items = els.slice().sort((a, b) => (b.rect.w * b.rect.h) - (a.rect.w * a.rect.h));
  const flat = layout ? flatten(layout) : [];     // ⭐ 布局树（拍平后做"认领"）

  const built: Array<{ el: PerceptionElement; node: CuNode }> = [];
  for (const el of items) {
    const node = makeNode(el.type, el.text, undefined, el.rect);
    // ⭐ 认领：把"像素上的这个框"挂到布局里**能覆盖它的最小**节点上 ⇒ 得到 layout 路径。
    //   认不到就显式标 degraded，运行期才允许它退到像素族 locator。
    const claim = flat.length ? claimLayout({ rect: el.rect, type: el.type }, flat) : null;
    if (claim) node.layout = claim.path;
    else node.degraded = true;

    let host: CuNode = root;
    let hostArea = -1;
    for (const b of built) {
      if (!CONTAINER_TYPES[String(b.el.type).toLowerCase()]) continue;
      if (coverRatio(b.el.rect, el.rect) < 0.85) continue;    // 装不下就不算父节点
      const area = b.el.rect.w * b.el.rect.h;
      if (hostArea < 0 || area < hostArea) { host = b.node; hostArea = area; }
    }
    host.append(node);
    built.push({ el, node });
  }

  if (layout) {
    if (flat.length) root.layout = flat[0].path;              // 根 = 布局树的根（窗口）
    root.fingerprint = fingerprintOf(layout);                 // 页面指纹（运行期判页用）
  } else {
    root.degraded = true;                                     // 没有布局通道 ⇒ 整树显式退化
  }
  return root;
}

// ————————————————————————————————————————————————
// 模型序列化（JS 侧固化 / 还原）
// ————————————————————————————————————————————————
interface SerNode {
  type: string; label: string;
  locator?: Locator; rect?: Rect;
  /** ⭐ 布局路径（运行期寻址依据）—— 固化时**必须**带上，否则运行期只剩像素一条路。 */
  layout?: LayoutPath;
  /** 显式退化标记（无可访问布局，只能像素族兜底）。 */
  degraded?: boolean;
  /** 页面指纹（只写在根上）：运行期判"还在不在同一页"。 */
  fingerprint?: PageFingerprint;
  children: SerNode[];
}

function serialize(n: ICuNode): SerNode {
  const self = n as CuNode;
  const out: SerNode = {
    type: n.type, label: n.label,
    locator: self.locator, rect: self.rect,
    children: n.children.map((c) => serialize(c as CuNode)),
  };
  if (self.layout) out.layout = self.layout;
  if (self.degraded) out.degraded = self.degraded;
  const fp = (n as any).fingerprint as PageFingerprint | undefined;
  if (fp) out.fingerprint = fp;
  return out;
}

class GenericNode extends CuNode {
  readonly type: string;
  constructor(t: string, l: string, o?: NodeOpts) { super(l, o); this.type = t; }
}

function makeNode(type: string, label: string, locator?: Locator, rect?: Rect,
                  layout?: LayoutPath, degraded?: boolean): CuNode {
  const o: NodeOpts = { locator, rect, layout, degraded };
  let n: CuNode;
  switch (type) {
    case 'app': n = new App(label, o); break;
    case 'tab': n = new Tab(label, locator); break;
    case 'menu': n = new Menu(label, locator); break;
    case 'toolbar': n = new Toolbar(label, locator); break;
    case 'group': n = new Group(label, locator); break;
    case 'window': n = new Window(label, locator); break;
    case 'canvas': n = new Canvas(label, locator); break;
    case 'button': n = new Button(label, locator!); break;
    case 'edit': n = new Edit(label, locator!); break;
    case 'combobox': n = new ComboBox(label, locator!); break;
    case 'checkbox': n = new CheckBox(label, locator!); break;
    case 'label': n = new Label(label, locator); break;
    case 'slider': n = new Slider(label, locator!); break;
    case 'link': n = new Link(label, locator!); break;
    default: n = new GenericNode(type, label, o); break;
  }
  // ⚠ 点击型控件(Button/Edit/CheckBox/...)的构造器只收 locator ⇒ 直接 return 会**丢掉 rect**。
  //   反序列化和"感知建树"都走这里(感知给的正是 rect), 所以统一在这里补。
  if (locator !== undefined) n.locator = locator;
  if (rect !== undefined) n.rect = rect;
  if (layout !== undefined) n.layout = layout;
  if (degraded !== undefined) n.degraded = degraded;
  return n;
}

function deserialize(s: SerNode): CuNode {
  const root = makeNode(s.type, s.label, s.locator, s.rect, s.layout, s.degraded);
  if (s.fingerprint) (root as any).fingerprint = s.fingerprint;
  for (const c of s.children) root.append(deserialize(c));
  return root;
}

// 重新导出类型，方便消费方 `import type { Rect, IButton } from 'computeruse'`
export type {
  Rect, Point, MouseBtn, Presence, Locator, ExploreOptions, ModelSnapshot,
  Perception, PerceptionElement, VlmOptions,
  ComputerUseBackend, ICuNode, IButton, ITab, IMenu, IToolbar, IGroup, IWindow,
  ICanvas, ILabel, IEdit, IComboBox, ICheckBox, ISlider, ILink, IApp,
  // ⭐ 布局通道 / 运行期寻址 / 本地分级（docs/compute-use-design.md §4.1、§1.3）
  LayoutPath, LayoutNode, PageFingerprint, AttachResult, NodeState, ExpectMatcher,
  SyncOptions, LaunchOptions, PerceptionReview, CuErrorCode,
  FailureKind, FailureAction, Diagnosis, DiagnoseContext,
} from './types';

// 感知层（可换后端：VLM/OCR/检测都在 perceive.ts 这一个文件里）
export { perceive, buildPrompt, setVlmConfig, vlmConfig } from './perceive';

// 布局工具（路径 / 拍平 / 查找 / 指纹 / 认领）与本地分级（偶发 vs 真错）
export {
  flatten, nodeAt, parentPath, fingerprintOf, fingerprintMatch, samePage, extraTopPaths, claimLayout,
  coverRatio as layoutCoverRatio,
} from './layout';
export type { FlatEntry } from './layout';
export { diagnose, formatDiagnosis, formatEvidence, stateMatches, explainMismatch } from './diagnose';
export type { EvidencePack } from './diagnose';
