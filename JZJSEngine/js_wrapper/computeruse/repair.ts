/// <reference path="../types/globals.d.ts" />
/// <reference path="../types/fetch.d.ts" />
// computeruse/repair.ts —— 界面不匹配的**升级路径**：把证据包交给大模型 → 拿回修法 → 落 override
//
// 分工（见 docs/compute-use-design.md §1.3）：
//   ① 本地先判（diagnose.ts）：偶发 / 弹窗 / 前置缺失都能自己处理 ⇒ **根本不叫模型**；
//   ② 只有"**稳定复现、但不知道该怎么修**"（page-miss / path-drift）才走到这里；
//   ③ 模型只写 **override**（本次生效）；模型文件运行期**只读** ——
//      要进模型必须显式合并（人工确认 / 再建模），否则脚本会自我漂移，就不可复现了。
import type {
  LayoutPath, RepairAnswer, RepairOptions, RepairResult, RepairTarget,
} from './types';
import { flatten } from './layout';
import { formatDiagnosis, formatEvidence } from './diagnose';
import { extractJson } from './perceive';

declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

const INSTRUCTIONS = [
  '你是"界面自动化脚本"的现场修复模块。脚本在运行期找不到某个控件，给你一次失败的证据包。',
  '',
  '证据包里有：期望（该步骤的断言 + 建模期记下的页面指纹）、实际（**当前可访问布局树**）、历史（重试次数/上次结论）。',
  '',
  '请判断这是哪一类，并给出本地能执行的处置：',
  '- transient：偶发（界面还没渲染完 / 正在切换）→ action=retry',
  '- popup：多出了对话框挡路 → action=dismiss-popup',
  '- precondition：控件在但还不能用（被禁用/状态不符）→ action=add-precondition',
  '- page-miss：**走错流程或换了界面**（页面指纹对不上）→ action=ask-model，newPath 留空',
  '- path-drift：**同一页里控件的路径变了**（版本/布局漂移）→ action=ask-model，',
  '  并从当前布局树里找出"那个控件**现在**的路径"填进 newPath',
  '',
  '⛔ newPath 只能从"当前布局树"里已有路径里**原样复制**，不要自己编造、不要拼不存在的层级。',
  '',
  '只输出 JSON（不要解释、不要 markdown 围栏）：',
  '{"kind":"path-drift","action":"ask-model","newPath":"/main/btnOutput","reason":"一句话"}',
].join('\n');

function buildUserText(pack: string, want: string): string {
  return ['# 失败的目标路径: ' + want, '', pack].join('\n');
}

/**
 * ⭐ 界面不匹配 → 本地分级 → （只有真错才）升级大模型 → 把修法落进 override。
 *
 * 返回值里 `localOnly=true` 表示"本地就判完了，没打扰模型" —— 这是**常态**，不是异常。
 */
export async function escalate(app: RepairTarget, err: unknown,
                               opts: RepairOptions): Promise<RepairResult> {
  // ① 本地分级（diagnose 会先拉一次当前布局）
  const d = await app.diagnose(err, {
    path: opts.path, expect: opts.expect,
    attempt: opts.attempt, maxAttempts: opts.maxAttempts,
  });

  // ② 本地能处理的，就不叫模型（偶发/弹窗/前置缺失）
  if (d.action !== 'ask-model')
    return { diagnosis: d, localOnly: true, answer: {}, applied: false };

  if (!opts.url)
    throw new Error('[CU] escalate: 缺模型端点 opts.url（本地判定=' + formatDiagnosis(d) + '）');

  // ③ 证据包：期望 + 实际布局树 + 历史（截图按需另带；这里不带 —— 判定不需要像素）
  const layout = app.getLayout();
  const evidence = formatEvidence({
    step: opts.step, path: opts.path, expect: opts.expect,
    attempt: opts.attempt, maxAttempts: opts.maxAttempts,
    diagnosis: d, fingerprint: app.fingerprint || null, layout, history: opts.history,
  });

  const body: Record<string, unknown> = {
    model: opts.model || 'default',
    messages: [{ role: 'user', content: buildUserText(evidence, opts.path) }],
    max_tokens: opts.maxTokens || 1024,
    temperature: 0,
    chat_template_kwargs: { enable_thinking: false },   // 关思考（不然 content 空、token 烧推理）
  };
  if (opts.prompt) body.messages = [{ role: 'user', content: opts.prompt + '\n\n' + evidence + '\n\n# 失败的目标路径: ' + opts.path }];
  if (opts.extraBody) for (const k in opts.extraBody) body[k] = (opts.extraBody as any)[k];

  const headers: Record<string, string> = { 'Content-Type': 'application/json' };
  if (opts.apiKey) headers['Authorization'] = 'Bearer ' + opts.apiKey;

  const res = await fetch(opts.url, {
    method: 'POST', headers, body: JSON.stringify(body), timeout: opts.timeoutMs || 600000,
  });
  const text = await res.text();
  if (!res.ok) throw new Error('[CU] escalate: 模型请求失败(HTTP ' + res.status + '): ' + text.slice(0, 300));

  let content = '';
  try {
    const data = JSON.parse(text);
    const msg = (data && data.choices && data.choices[0] && data.choices[0].message) || {};
    content = typeof msg.content === 'string' ? msg.content : '';
  } catch (e) { /* 下面按"没内容"处理 */ }
  if (!content.trim()) throw new Error('[CU] escalate: 模型没返回内容（是否开着思考？用 extraBody 关掉）');

  const doc: any = extractJson(content);
  const answer: RepairAnswer = {
    kind: doc && doc.kind, action: doc && doc.action,
    newPath: doc && doc.newPath ? String(doc.newPath) : '',
    reason: doc && doc.reason,
  };

  // ④ ⭐ 只接受"**当前布局树里确实存在**"的路径：模型编一个不存在的路径，宁可判失败也不写
  let applied = false;
  let ov: { from: LayoutPath; to: LayoutPath } | undefined;
  if (answer.newPath) {
    const exists = !!layout && flatten(layout).some((e) => e.path === answer.newPath);
    if (exists) {
      app.override[opts.path] = answer.newPath;
      applied = true;
      ov = { from: opts.path, to: answer.newPath };
    } else {
      console.warn('[CU] escalate: 模型给的路径在当前布局树里不存在, 不采纳: ' + answer.newPath);
    }
  }

  console.log('[CU] escalate: ' + formatDiagnosis(d)
    + ' ⇒ 模型{' + (answer.kind || '?') + ' / ' + (answer.action || '?') + '}'
    + (applied ? ' override ' + ov!.from + ' → ' + ov!.to : ' (未采纳新路径)'));
  return { diagnosis: d, localOnly: false, answer, applied, override: ov, evidence, raw: content };
}
