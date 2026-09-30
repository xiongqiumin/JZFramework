// computeruse/diagnose.ts —— **界面不匹配的本地分级**（见 docs/compute-use-design.md §1.3）
//
// 核心判据：**"偶发 vs 真错"由"重试后是否稳定复现"决定，不靠模型拍脑袋**。
//   重试就好了 ⇒ 偶发（A 时序 / C 弹窗），本地自己处理；
//   稳定复现才是错（B 前置缺失 / D 走错页 / E 版本漂移 / F 进程没了），这时才升级大模型。
//
// 所以本文件做的事只有三件：
//   ① 把"预期 vs 实际"说清楚（`explainMismatch`）；
//   ② 用**本地证据**（重试次数 / 布局树 / 页面指纹 / enabled·checked·range）给出 `Diagnosis`；
//   ③ 给出**本地能执行的处置**（重试 / 等 / 关弹窗 / 补前置 / 问模型 / 重启 / 中止）。
// 全程在**布局通道**上判定，不截图、不 OCR（像素只在退化为兜底时才出现）。
import type {
  Diagnosis, DiagnoseContext, ExpectMatcher, FailureAction, FailureKind,
  LayoutNode, LayoutPath, NodeState,
} from './types';
import { extraTopPaths, nodeAt, samePage } from './layout';

// ————————————————————————————————————————————————
// 期望 vs 实际
// ————————————————————————————————————————————————

function textOk(actual: string, want: string | RegExp): boolean {
  const a = actual === undefined || actual === null ? '' : String(actual);
  if (want instanceof RegExp) return want.test(a);
  return a === String(want);
}

/** 状态是否满足期望。 */
export function stateMatches(state: NodeState, matcher: ExpectMatcher): boolean {
  if (matcher === null || matcher === undefined) return true;
  if (typeof matcher === 'string' || typeof matcher === 'number' || typeof matcher === 'boolean') {
    if (typeof matcher === 'number') return !!state.range && state.range.value === matcher;
    if (typeof matcher === 'boolean') return state.checked === matcher;
    return textOk(state.text || '', matcher);
  }
  if (matcher instanceof RegExp) return textOk(state.text || '', matcher);

  if (matcher.exists !== undefined && state.exists !== matcher.exists) return false;
  if (matcher.text !== undefined && !textOk(state.text || '', matcher.text)) return false;
  if (matcher.checked !== undefined && state.checked !== matcher.checked) return false;
  if (matcher.enabled !== undefined && state.enabled !== matcher.enabled) return false;
  if (matcher.range) {
    if (!state.range) return false;
    if (matcher.range.value !== undefined && state.range.value !== matcher.range.value) return false;
    if (matcher.range.min !== undefined && state.range.min !== matcher.range.min) return false;
    if (matcher.range.max !== undefined && state.range.max !== matcher.range.max) return false;
  }
  return true;
}

/** 人话说明"差在哪"（断言失败与升级模型的证据包都用它）。 */
export function explainMismatch(state: NodeState, matcher: ExpectMatcher): string {
  const a = '实际{' + [
    'exists=' + state.exists,
    'from=' + state.from,
    state.text !== undefined ? 'text=' + JSON.stringify(state.text) : '',
    state.checked !== undefined ? 'checked=' + state.checked : '',
    state.enabled !== undefined ? 'enabled=' + state.enabled : '',
    state.range ? 'range=' + state.range.value + '[' + state.range.min + ',' + state.range.max + ']' : '',
  ].filter(Boolean).join(' ') + '}';
  const w = '期望{' + (matcher instanceof RegExp ? String(matcher) : JSON.stringify(matcher)) + '}';
  return a + ' ≠ ' + w;
}

// ————————————————————————————————————————————————
// 分级
// ————————————————————————————————————————————————

interface ErrInfo { code: string; message: string; }

function errInfo(err: unknown): ErrInfo {
  const e: any = err || {};
  const message = e.message ? String(e.message) : String(err);
  let code = typeof e.code === 'string' ? e.code : '';
  if (!code) {                                   // 兜底：老式错误只能认消息
    if (/未找到|不存在|not found|not-found/i.test(message)) code = 'not-found';
    else if (/断言|expect/i.test(message)) code = 'expect-failed';
    else if (/窗口|进程|gone/i.test(message)) code = 'window-gone';
  }
  return { code, message };
}

function usableReason(node: LayoutNode): string {
  if (node.enabled === false) return '节点在布局里，但 enabled=false（前置条件未满足）';
  if (node.visible === false) return '节点在布局里，但 visible=false（还不可见）';
  return '';
}

export function diagnose(err: unknown, ctx?: DiagnoseContext): Diagnosis {
  const c: DiagnoseContext = ctx || {};
  const info = errInfo(err);
  const attempt = c.attempt && c.attempt > 0 ? c.attempt : 1;
  const maxAttempts = c.maxAttempts && c.maxAttempts > 0 ? c.maxAttempts : 3;
  const layout: LayoutNode | null = c.layout || null;
  const make = (kind: FailureKind, transient: boolean, action: FailureAction,
                reason: string, evidence: Diagnosis['evidence']): Diagnosis =>
    ({ kind, transient, action, reason, evidence });

  // ① 终端类：本地没有任何可做的事
  if (info.code === 'unsupported')
    return make('none', false, 'abort', '后端没有实现该原语（不是界面问题）：' + info.message, {});
  if (info.code === 'window-gone')
    return make('process-gone', false, 'restart', '目标窗口/进程已消失：重启并回到可恢复点，别往下点', { path: c.path });

  // ② 没有布局通道 ⇒ 连"页面变没变"都判不了（只能像素兜底）
  if (!layout)
    return make('no-layout', false, 'ask-model',
      '拿不到可访问布局：该目标只能走像素族兜底（模型里节点应标 degraded），无法本地判页',
      { path: c.path });

  const live = c.path ? nodeAt(layout, c.path) : null;
  const samePageNow = samePage(c.fingerprint, layout);
  // 弹窗判据（纯结构，得小心别把"改名的控件"当成弹窗）：
  //   **同一页**（大部分关键路径还在）**且**期望的关键路径**一个都没少**（该在的都在）
  //   **且**多出了顶层节点 ⇒ 那确实是"多出来的东西"（对话框/提示）。
  // 反例：控件改 viewId 时也会多出一个没见过的路径 —— 那种情况上面第二条不成立（少了一个），
  //   于是归到 path-drift（修法完全不同：漂移=改路径，弹窗=关掉它）。
  const missingKeyPaths = (c.fingerprint && c.fingerprint.paths ? c.fingerprint.paths : [])
    .filter((p) => !nodeAt(layout, p));
  const extras = (samePageNow && missingKeyPaths.length === 0)
    ? extraTopPaths(c.fingerprint, layout) : [];

  // ③ 弹窗最优先（本地能自己处理，不必等重试、更不必问模型）
  if (extras.length)
    return make('popup', true, 'dismiss-popup',
      '布局里多出顶层节点（疑似弹窗/对话框）：本地关掉它再重试',
      { path: c.path, extraPaths: extras });

  // ③ 路径在布局里：判"这一页是不是还在 / 这个控件能不能用"
  if (live) {
    const bad = usableReason(live);
    if (bad) {
      if (attempt < maxAttempts)
        return make('transient', true, 'wait',
          '第 ' + attempt + '/' + maxAttempts + ' 次：' + bad + ' ⇒ 先等界面更新再重试', { path: c.path });
      return make('precondition', false, 'add-precondition', bad, { path: c.path, foundButUnusable: c.path });
    }
    if (info.code === 'expect-failed') {
      if (attempt < maxAttempts)
        return make('transient', true, 'wait',
          '第 ' + attempt + '/' + maxAttempts + ' 次状态不符（界面可能还没更新完）⇒ 等稳定后重试', { path: c.path });
      return make('precondition', false, 'ask-model',
        '稳定复现的状态不符：查前置条件或该步是否真的成功', { path: c.path, foundButUnusable: c.path });
    }
    // 路径在、状态也能用，却报错了 ⇒ 大概率是时序（注入失败/窗口没激活）
    if (attempt < maxAttempts)
      return make('transient', true, 'retry', '第 ' + attempt + '/' + maxAttempts + ' 次失败：原样重试', { path: c.path });
    return make('precondition', false, 'ask-model', '稳定复现但异常不明：' + info.message, { path: c.path });
  }

  // ④ 路径不在布局里：先看是不是偶发（等一等/重试就好）
  if (attempt < maxAttempts)
    return make('transient', true, 'wait',
      '第 ' + attempt + '/' + maxAttempts + ' 次找不到：可能还没渲染出来 / 界面正在切换 ⇒ 等稳定后重试',
      { path: c.path, missingPath: c.path });

  const fpMatched = samePageNow;
  if (c.fingerprint && !fpMatched)
    return make('page-miss', false, 'ask-model',
      '重试后仍找不到，且**页面指纹对不上**（走错流程或换了界面）⇒ 升级模型：现在是什么界面、该走哪条路',
      { path: c.path, fingerprintChanged: true, missingPath: c.path, extraPaths: extras });

  if (c.fingerprint)
    return make('path-drift', false, 'ask-model',
      '同一页里节点路径变了（版本/布局漂移）⇒ 升级模型：给出该控件的新路径，更新映射',
      { path: c.path, fingerprintChanged: false, missingPath: c.path });

  return make('path-drift', false, 'ask-model',
    '稳定复现的路径缺失（没有指纹可比 ⇒ 无从判页）⇒ 升级模型核对界面',
    { path: c.path, missingPath: c.path });
}

/** 把 `Diagnosis` 压成一行，方便日志/回执。 */
export function formatDiagnosis(d: Diagnosis): string {
  return '[CU] ' + d.kind + (d.transient ? '(偶发)' : '(真错)')
    + ' → ' + d.action + ' :: ' + d.reason;
}

/** 升级大模型时的**证据包**（期望 + 实际 + 历史；截图按需另带）。 */
export interface EvidencePack {
  step?: string;
  path?: LayoutPath;
  expect?: ExpectMatcher;
  attempt?: number;
  maxAttempts?: number;
  diagnosis: Diagnosis;
  /** 模型里记的期望指纹。 */
  fingerprint?: unknown;
  /** 当前布局树（原样给模型，它才看得出"少了什么/多了什么"）。 */
  layout?: LayoutNode | null;
  /** 已重试次数与上次模型结论（判"是不是又在重复没用的答案"）。 */
  history?: { retries?: number; lastModelAnswer?: string };
}

/** 证据包 → 文本（给模型/日志；**不截屏**，除非调用方另附截图）。 */
export function formatEvidence(pack: EvidencePack): string {
  const lines: string[] = [];
  lines.push('# 步骤: ' + (pack.step || '(未命名)'));
  lines.push('# 目标路径: ' + (pack.path || '(无)'));
  lines.push('# 期望: ' + (pack.expect === undefined ? '(未声明)'
    : (pack.expect instanceof RegExp ? String(pack.expect) : JSON.stringify(pack.expect))));
  lines.push('# 重试: ' + (pack.history && pack.history.retries !== undefined ? pack.history.retries : (pack.attempt || 1) - 1)
    + '/' + (pack.maxAttempts || 3));
  lines.push('# 本地判定: ' + formatDiagnosis(pack.diagnosis));
  if (pack.history && pack.history.lastModelAnswer)
    lines.push('# 上次模型结论: ' + pack.history.lastModelAnswer);
  lines.push('# 期望指纹: ' + JSON.stringify(pack.fingerprint || null));
  lines.push('# 当前布局树:');
  lines.push(JSON.stringify(pack.layout || null, null, 1));
  return lines.join('\n');
}
