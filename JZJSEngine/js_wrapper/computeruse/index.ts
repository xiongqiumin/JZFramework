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
} from './types';
import { perceive } from './perceive';

declare const __jz: any;

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
  // 模型固化在 JS 侧(序列化本树 JSON), 经 __jz.core 落盘
  saveModel: async (app, path) => { cuCore().writeFile(path, JSON.stringify(serialize(app), null, 2)); },
  loadModel: async (path) => {
    const data = cuCore().readFile(path);
    const text = typeof data === 'string' ? data : new TextDecoder().decode(data);
    return deserialize(JSON.parse(text)) as unknown as IApp;
  },
  // 探索 = "看一眼" + "装配"两步: ① 截一张**客户区**像素图; ② 感知后端(见 perceive.ts)给出元素清单;
  // ③ 元素清单 → 控件树(纯装配, 不碰屏幕)。感知后端可换(离线/VLM/OCR), 这一步的契约不变。
  explore: async (opts: ExploreOptions): Promise<IApp> => {
    const shot = await backend.screenshot();
    const p = await perceive(shot, opts ? { vlm: opts.vlm } : undefined);
    return buildTree(p);
  },
  fromPerception: async (tree: unknown): Promise<IApp> => buildTree(tree as Perception),
};

export function setBackend(b: ComputerUseBackend): void { backend = b; }

let _seq = 0;
function genId(): string { return 'cu' + (++_seq); }

interface NodeOpts { locator?: Locator; rect?: Rect; }

// ————————————————————————————————————————————————
// 节点基类：所有控件共用"定位→点击"语义
// ————————————————————————————————————————————————
export abstract class CuNode implements ICuNode {
  id: string;
  abstract readonly type: string;
  label: string;
  rect?: Rect;
  locator?: Locator;
  presence?: Presence;
  parent?: CuNode;
  children: CuNode[] = [];

  constructor(label: string, opts?: NodeOpts) {
    this.label = label;
    this.id = genId();
    if (opts) { this.locator = opts.locator; this.rect = opts.rect; }
  }

  protected add<T extends CuNode>(n: T): T { n.parent = this; this.children.push(n); return n; }
  append(child: CuNode): CuNode { child.parent = this; this.children.push(child); return child; }

  async activate(): Promise<void> {}
  async ensureActivated(): Promise<void> {
    if (this.parent) await this.parent.ensureActivated();
    await this.activate();
  }
  // 解析为屏幕真实坐标：优先 locator（按策略在 region 内查找），其次 rect。
  async locate(): Promise<Rect | null> {
    if (this.locator) return backend.match(this.locator);
    if (this.rect) return this.rect;
    return null;
  }
  async exists(): Promise<boolean> { return (await this.locate()) !== null; }

  // 通用点击：先判存在，再注入；失败抛异常。Button/Link/WebElement 直接复用。
  async click(btn: MouseBtn = 'left'): Promise<void> {
    const hit = await this.locate();
    if (!hit) throw new Error(`[CU] 控件 "${this.label}" 未找到（不存在）`);
    const ok = await backend.click(hit, btn);
    if (!ok) throw new Error(`[CU] 点击失败: "${this.label}"`);
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
  constructor(name: string, opts?: NodeOpts) { super(name, opts); }

  find(type: string, label: string): CuNode | undefined {
    const stack: CuNode[] = [this];
    while (stack.length) {
      const n = stack.pop()!;
      if (n.type === type && n.label === label) return n;
      for (const c of n.children) stack.push(c);
    }
    return undefined;
  }
  button(l: string): IButton {
    const n = this.find('button', l);
    if (!n) throw new Error(`[CU] 模型中不存在按钮: "${l}"`);
    return n as unknown as IButton;
  }
  async save(path: string): Promise<void> { return backend.saveModel(this, path); }

  static async load(path: string): Promise<IApp> { return backend.loadModel(path); }
  static async explore(opts: ExploreOptions): Promise<IApp> { return backend.explore(opts); }
  static async fromPerception(tree: unknown): Promise<IApp> { return backend.fromPerception(tree); }
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
function buildTree(p: Perception | null): IApp {
  const root = new App('');                       // ⚠ 见上：根节点不带文本（真值同样约定为空串）
  const els: PerceptionElement[] = (p && p.elements) ? p.elements : [];
  const items = els.slice().sort((a, b) => (b.rect.w * b.rect.h) - (a.rect.w * a.rect.h));

  const built: Array<{ el: PerceptionElement; node: CuNode }> = [];
  for (const el of items) {
    const node = makeNode(el.type, el.text, undefined, el.rect);
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
  return root;
}

// ————————————————————————————————————————————————
// 模型序列化（JS 侧固化 / 还原）
// ————————————————————————————————————————————————
interface SerNode { type: string; label: string; locator?: Locator; rect?: Rect; children: SerNode[]; }

function serialize(n: ICuNode): SerNode {
  const self = n as CuNode;
  return {
    type: n.type, label: n.label,
    locator: self.locator, rect: self.rect,
    children: n.children.map((c) => serialize(c as CuNode)),
  };
}

class GenericNode extends CuNode {
  readonly type: string;
  constructor(t: string, l: string, o?: NodeOpts) { super(l, o); this.type = t; }
}

function makeNode(type: string, label: string, locator?: Locator, rect?: Rect): CuNode {
  const o: NodeOpts = { locator, rect };
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
  return n;
}

function deserialize(s: SerNode): CuNode {
  const root = makeNode(s.type, s.label, s.locator, s.rect);
  for (const c of s.children) root.append(deserialize(c));
  return root;
}

// 重新导出类型，方便消费方 `import type { Rect, IButton } from 'computeruse'`
export type {
  Rect, Point, MouseBtn, Presence, Locator, ExploreOptions, ModelSnapshot,
  Perception, PerceptionElement, VlmOptions,
  ComputerUseBackend, ICuNode, IButton, ITab, IMenu, IToolbar, IGroup, IWindow,
  ICanvas, ILabel, IEdit, IComboBox, ICheckBox, ISlider, ILink, IApp,
} from './types';

// 感知层（可换后端：VLM/OCR/检测都在 perceive.ts 这一个文件里）
export { perceive, buildPrompt, setVlmConfig, vlmConfig } from './perceive';
