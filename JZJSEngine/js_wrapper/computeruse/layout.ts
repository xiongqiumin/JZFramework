// computeruse/layout.ts —— **布局树**的通用工具：路径生成 / 拍平 / 查找 / 指纹 / 覆盖率
//
// 为什么单开一个文件：路径与指纹是"建模期建映射、运行期寻址与判页"共用的基础件，
// 放在 index.ts 里会和控件树逻辑缠在一起，放在 diagnose.ts 里又会让"分级"依赖"布局"细节。
// 本文件只依赖 types.ts（没有循环依赖）。
import type { LayoutNode, LayoutPath, PageFingerprint, Rect } from './types';

/** 拍平后的一项：节点 + 它的布局路径 + 深度（父在子前）。 */
export interface FlatEntry {
  node: LayoutNode;
  /** 如 `/main/btnOutput`。 */
  path: LayoutPath;
  depth: number;
}

/** 路径片段：优先 `viewId`(= objectName())，没有就用 role。 */
function segmentOf(n: LayoutNode): string {
  const seg = String(n.id || n.role || 'node').trim();
  return seg === '' ? 'node' : seg.replace(/[/\\]/g, '_');
}

/** 同一父下重名时补 `#2`、`#3`…（`/main/button#2`）——路径必须唯一，否则寻址会歧义。 */
function uniqueSegment(base: string, used: Record<string, number>): string {
  const k = (used[base] = (used[base] || 0) + 1);
  return k > 1 ? base + '#' + k : base;
}

/**
 * 把布局树拍平成 `[{node, path, depth}]`（父在子前）。
 * 路径 = 从根往下拼路径片段；根就是 `/<seg>`。
 */
export function flatten(root?: LayoutNode | null): FlatEntry[] {
  const out: FlatEntry[] = [];
  if (!root) return out;

  const rootPath = '/' + segmentOf(root);
  out.push({ node: root, path: rootPath, depth: 0 });

  const walk = (parent: LayoutNode, parentPath: string, depth: number): void => {
    const kids: LayoutNode[] = parent.children || [];
    const used: Record<string, number> = {};
    for (const child of kids) {
      if (!child || typeof child !== 'object') continue;
      const path = parentPath + '/' + uniqueSegment(segmentOf(child), used);
      out.push({ node: child, path, depth: depth + 1 });
      walk(child, path, depth + 1);
    }
  };
  walk(root, rootPath, 0);
  return out;
}

/** 路径 → 节点（找不到返回 null）。 */
export function nodeAt(root: LayoutNode | null | undefined, path: LayoutPath): LayoutNode | null {
  if (!root || !path) return null;
  for (const e of flatten(root)) if (e.path === path) return e.node;
  return null;
}

/** 节点的父路径（`/a/b/c` → `/a/b`；根返回空串）。 */
export function parentPath(path: LayoutPath): string {
  const i = String(path || '').lastIndexOf('/');
  return i <= 0 ? '' : String(path).slice(0, i);
}

/**
 * 页面指纹：窗口标识 + **关键节点路径集合**。用来判"还在不在同一页"。
 *
 * "关键"= 有 `id`(viewId) 的可见节点（这些是建模期就有稳定标识的），最多取 `max` 个；
 * 一个都没有时退化为"前 max 个路径"，免得指纹为空 ⇒ 判页退化成"永远不匹配"。
 */
export function fingerprintOf(root: LayoutNode | null | undefined,
                             opts?: { max?: number; window?: string }): PageFingerprint {
  const max = (opts && opts.max) || 24;
  const flat = flatten(root);
  const key = flat.filter((e) => e.node.id && e.node.visible !== false).map((e) => e.path);
  const paths = (key.length ? key : flat.map((e) => e.path)).slice(0, max);

  let win = opts && opts.window;
  if (!win && root) win = String(root.text || root.name || root.id || root.role || '');
  return { window: win, paths };
}

/** 记的指纹里有多少路径还在当前布局里（[0,1]）。指纹为空返回 0。 */
export function fingerprintMatch(recorded: PageFingerprint | null | undefined,
                                live: LayoutNode | null | undefined): number {
  if (!recorded || !recorded.paths || !recorded.paths.length) return 0;
  const livePaths: Record<string, boolean> = {};
  for (const e of flatten(live)) livePaths[e.path] = true;
  let hit = 0;
  for (const p of recorded.paths) if (livePaths[p]) hit++;
  return hit / recorded.paths.length;
}

/**
 * 与记的指纹比：≥ 阈值视为"还在同一页"。
 *
 * ⚠ 默认阈值取 **0.6**（不是 0.8）：关键节点集合通常很小（几个到二十几个），
 *   一两个控件改了 viewId 不该被当成"换页" —— 那正是 path-drift（同页漂移）的典型形状；
 *   换页的特征是"**大部分**关键节点都不见了"。阈值取高会把两种真错混成一种，
 *   而它们的修法完全不同（漂移=改路径；换页=重走流程）。
 */
export function samePage(recorded: PageFingerprint | null | undefined,
                         live: LayoutNode | null | undefined,
                         threshold?: number): boolean {
  return fingerprintMatch(recorded, live) >= (threshold === undefined ? 0.6 : threshold);
}

/** 布局里"多出来的顶层节点"（疑似弹窗）：深度 ≤ 1 且不在记的指纹里。 */
export function extraTopPaths(recorded: PageFingerprint | null | undefined,
                              live: LayoutNode | null | undefined): LayoutPath[] {
  const known: Record<string, boolean> = {};
  if (recorded && recorded.paths) for (const p of recorded.paths) known[p] = true;
  const out: LayoutPath[] = [];
  for (const e of flatten(live)) {
    if (e.depth > 1) continue;
    if (known[e.path]) continue;
    out.push(e.path);
  }
  return out;
}

/** `inner` 被 `outer` 覆盖的面积占比（[0,1]，矩形包含率）。 */
export function coverRatio(outer: Rect, inner: Rect): number {
  if (!outer || !inner) return 0;
  const x0 = Math.max(outer.x, inner.x), y0 = Math.max(outer.y, inner.y);
  const x1 = Math.min(outer.x + outer.w, inner.x + inner.w);
  const y1 = Math.min(outer.y + outer.h, inner.y + inner.h);
  const iw = x1 - x0, ih = y1 - y0;
  const area = inner.w * inner.h;
  if (iw <= 0 || ih <= 0 || area <= 0) return 0;
  return (iw * ih) / area;
}

/**
 * 为"感知元素"认领布局节点：在所有**覆盖**它的布局节点里取**最小**的那个（层级推断同口径）。
 * `minCover` 默认 0.6 —— 感知框与布局框不会像素级重合，留足余量；宁缺勿错。
 */
export function claimLayout(entry: { rect: Rect; type?: string },
                            flat: FlatEntry[],
                            opts?: { minCover?: number }): FlatEntry | null {
  const minCover = opts && opts.minCover !== undefined ? opts.minCover : 0.6;
  let best: FlatEntry | null = null;
  let bestArea = -1;
  for (const e of flat) {
    const r = e.node.rect;
    if (!r) continue;
    if (coverRatio(r, entry.rect) < minCover) continue;
    const area = r.w * r.h;
    if (bestArea < 0 || area < bestArea) { best = e; bestArea = area; }
  }
  return best;
}
