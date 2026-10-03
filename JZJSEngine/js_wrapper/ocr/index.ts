/// <reference path="../types/globals.d.ts" />
// ocr 包 —— 运行期实现：本地 PP-OCR（不依赖任何外部服务）在 TS 侧的门面。
//
// 真干活的全在 C++ 侧（JZComputerUse 的 OCR 那组，经 __jz.computeruse 暴露）：
//   · 它读的是**当前屏幕**（虚拟桌面下就是桌面合成帧），不碰文件；
//   · 识别结果是**物理像素**矩形 —— 与 layoutTree / click 同一口径 ⇒ 拿到就能点。
//
// 这一层只做三件事：把 `built`（有没有这个能力）与 `ready`（模型好没好）分开问清楚、
// 把"按文字找"做得比 C++ 的 matchText 更好用（支持包含/全等/正则、返回命中的文本与分数）、
// 把失败变成**可判定的空结果**而不是半真半假的成功。
//
// 典型用法（脚本里第一次用 OCR 前）：
//     import { ensure, texts, find, center } from 'ocr';
//     ensure();                                   // 没 init 就 init（模型目录走 JZ_CU_OCR_MODEL）
//     const hit = find('开始');                    // 包含匹配，不区分大小写
//     if (hit) clickTo(center(hit));              // 物理像素中心，直接点
//
// 模型目录：init({modelRoot}) > 环境变量 JZ_CU_OCR_MODEL > <exe目录>/model/paddle_ocr
// （本机模型在 E:\models\paddle_ocr，目录布局见 JZPaddleOCR::initFromDir 认的两套）

import type { Rect, TextHit, OcrInfo, FindOptions, TextsOptions, OcrBackend } from './types';

declare const __jz: any;
// 引擎注入的全局 console（本包不依赖 DOM/Node 类型库）
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

/** ocr 的错误（一般只在"拿后端"这类编程错误时抛；识别不到返回空/null 而不是抛） */
export class OcrError extends Error {
  code: 'noBackend';
  constructor(code: OcrError['code'], message: string) {
    super(message);
    this.name = 'OcrError';
    this.code = code;
  }
}

// OCR 那组挂在 __jz.computeruse 上（和点击/截图同一个模块）
function cu(): OcrBackend {
  const c = __jz && __jz.computeruse;
  if (!c)
    throw new OcrError('noBackend',
      '[ocr] 后端未加载：__jz.computeruse 不存在（宿主没注册 JZJSComputerUseModule）');
  return c as OcrBackend;
}

/** 本构建是否**编入了** OCR 能力。false ⇒ 后面所有调用都会是空结果（脚本据此降级）。 */
export function built(): boolean {
  const c = __jz && __jz.computeruse;
  if (!c) return false;
  if (typeof c.ocrBuilt === 'function') return !!c.ocrBuilt();
  // 老宿主没有 ocrBuilt：不敢说没有（那会把"能用的环境"误判成不能用），保守给 true，
  // 真实的可用性交给 init()/ready() 暴露。
  return true;
}

/** 诊断信息：{built, ready, cls, modelRoot}。排查"为什么识别不出字"先看这里。 */
export function info(): OcrInfo {
  const c = __jz && __jz.computeruse;
  if (!c)
    return { built: false, ready: false, cls: false, modelRoot: '',
             hint: '__jz.computeruse 不存在（宿主没注册 ComputerUse 模块）' };
  if (typeof c.ocrInfo === 'function') return c.ocrInfo() as OcrInfo;
  return { built: true, ready: !!c.ocrReady(), cls: false, modelRoot: '' };
}

/** 加载模型（慢，一次就够；重复调用会在已就绪时直接返回 true）。
 *  `modelRoot` 不给就按环境变量 / exe 旁的默认位置找。 */
export function init(opts?: { modelRoot?: string }): boolean {
  return !!cu().initOcr(JSON.stringify({ modelRoot: (opts && opts.modelRoot) || '' }));
}

/** 模型是否已就绪（`built()` 为假时恒为假） */
export function ready(): boolean {
  const c = __jz && __jz.computeruse;
  return !!(c && c.ocrReady());
}

/** 便捷：没就绪就先 init。返回最终是否就绪 —— 脚本里一行就够。 */
export function ensure(opts?: { modelRoot?: string }): boolean {
  return ready() || init(opts);
}

/** 释放模型占的内存（之后 ready() 变 false；下次用要重新 init） */
export function deinit(): void {
  const c = __jz && __jz.computeruse;
  if (c && typeof c.deinitOcr === 'function') c.deinitOcr();
}

/** 屏幕上（或指定区域内）所有文字。没就绪 / 没编入 ⇒ 空数组，不假装成功。 */
export function texts(opts?: TextsOptions): TextHit[] {
  if (!ready()) return [];
  const out = cu().ocrTexts(JSON.stringify(opts || {}));
  return (out || []) as TextHit[];
}

function matcherOf(text: string, opts?: FindOptions): (s: string) => boolean {
  if (opts && opts.regex) {
    const re = (opts.regex instanceof RegExp) ? opts.regex : new RegExp(text, 'i');
    return (s) => { re.lastIndex = 0; return re.test(s); };
  }
  const needle = text.toLowerCase();
  if (opts && opts.exact) return (s) => s.toLowerCase() === needle;
  return (s) => s.toLowerCase().indexOf(needle) >= 0;
}

/** 按文字找**全部**命中（按置信度降序）。匹配方式见 FindOptions。 */
export function findAll(text: string, opts?: FindOptions): TextHit[] {
  if (!text) return [];
  const match = matcherOf(text, opts);
  const hits = texts({ region: opts && opts.region, minScore: opts && opts.minScore })
    .filter((h) => !!h.text && match(h.text));
  hits.sort((a, b) => b.score - a.score);
  return hits;
}

/** 按文字找**最像的一个**；没找到返回 null（而不是抛错或返回零矩形）。 */
export function find(text: string, opts?: FindOptions): TextHit | null {
  const hits = findAll(text, opts);
  return hits.length ? hits[0] : null;
}

/** **物理像素**中心 —— 直接可以喂给 computeruse 的 click/drag（同一口径） */
export function center(hit: TextHit): { x: number; y: number } {
  return { x: hit.x + hit.w / 2, y: hit.y + hit.h / 2 };
}

// ————————————————————————————————————————————————————————————
// default 导出：给 `import ocr from 'ocr'` 用（具名导入是主用法）
// ————————————————————————————————————————————————————————————
const ocr = { OcrError, built, info, init, ready, ensure, deinit, texts, find, findAll, center };
export default ocr;
