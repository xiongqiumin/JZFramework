/// <reference path="../types/globals.d.ts" />
// onnx 包 —— 运行期实现：把 ONNX Runtime 包成"加载模型 → 看输入输出 → 喂张量 → 拿张量"。
//
// 设计：
//   · 真干活的全在 C++ 侧（__jz.onnx，JZJSOnnxModule）：会话是 C++ 里的一张 **句柄表**，
//     JS 只拿一个 number。加载一次、反复推理，不用每次重新解析模型。
//   · 这一层只做两件事：把 feeds 整理成 JSON 送过去；把 {ok:false} 变成**抛错**。
//   · ⚠ 第一版数据走 JSON 数组（作者明确"先简单"）⇒ 大张量会慢。上万元素以上建议用
//     `dataB64`（原始字节）而不是 `data` 数组；再往后该做的是二进制通道与"按需取输出"。
//
// 典型用法（跑一个 1x3x32x32 输入的模型）：
//     import { load, available } from 'onnx';
//     if (!available()) { /* 这个 exe 没编 ONNX Runtime */ }
//     const s = load('E:/models/m.onnx');
//     console.log(s.inputs, s.outputs);              // 先看模型要什么、给什么
//     const out = s.run({ x: { shape: [1, 3, 32, 32], data: floats } });
//     const best = argmax(out[0]);
//     s.free();

import type { DType, AnyDType, TensorSpec, Feed, Tensor, OnnxBackend } from './types';

declare const __jz: any;
// 引擎注入的全局 console（本包不依赖 DOM/Node 类型库）
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

/** onnx 的错误：`code` 便于调用方分类处理，不靠错误文本判断。 */
export class OnnxError extends Error {
  code: 'noBackend' | 'notBuilt' | 'failed' | 'invalid' | 'released';
  constructor(code: OnnxError['code'], message: string) {
    super(message);
    this.name = 'OnnxError';
    this.code = code;
  }
}

function onnb(): OnnxBackend {
  if (!__jz || !__jz.onnx)
    throw new OnnxError('noBackend',
      '[onnx] 后端未加载：__jz.onnx 不存在（宿主没注册 JZJSOnnxModule）');
  return __jz.onnx as OnnxBackend;
}

/** 本构建是否编入了 ONNX Runtime。没编时其余调用都会抛 notBuilt。 */
export function available(): boolean {
  return !!(__jz && __jz.onnx && __jz.onnx.available());
}

/** ONNX Runtime 版本（没编入时是空串）。排查"模型加载不了"时第一个该看的东西。 */
export function version(): string {
  return available() ? onnb().version() : '';
}

function check(r: any, what: string): any {
  if (!r || r.ok !== true) {
    const msg = (r && r.error) ? String(r.error) : '未知错误';
    throw new OnnxError('failed', `[onnx] ${what} 失败: ${msg}`);
  }
  return r;
}

// ————————————————————————————————————————————————————————————
// Session —— 一个加载好的模型
// ————————————————————————————————————————————————————————————
export class Session {
  /** 会话句柄（C++ 侧那张表的键） */
  readonly id: number;
  /** 模型路径（记着，报错时好用） */
  readonly model: string;
  private freed = false;

  /** @internal */
  constructor(id: number, model: string) { this.id = id; this.model = model; }

  /** 模型声明的输入（名字/类型/形状；形状里的 -1 是动态维度） */
  get inputs(): TensorSpec[] { return this.infos().inputs; }
  /** 模型声明的输出 */
  get outputs(): TensorSpec[] { return this.infos().outputs; }

  private infos(): { inputs: TensorSpec[]; outputs: TensorSpec[] } {
    if (this.freed) throw new OnnxError('released', '[onnx] 这个会话已经被 free() 了');
    const r = check(onnb().info(this.id), 'info');
    return { inputs: (r.inputs || []) as TensorSpec[], outputs: (r.outputs || []) as TensorSpec[] };
  }

  /** 推理。feeds 的键必须是模型给的输入名；缺哪个就报哪个，不会用 0 偷偷填上。 */
  run(feeds: Record<string, Feed>): Tensor[] {
    if (this.freed) throw new OnnxError('released', '[onnx] 这个会话已经被 free() 了');
    for (const k of Object.keys(feeds || {})) {
      const f = feeds[k];
      if (!f || !f.shape || !f.shape.length)
        throw new OnnxError('invalid', `[onnx] feeds['${k}'] 缺少 shape`);
      if (!f.data && !f.dataB64)
        throw new OnnxError('invalid', `[onnx] feeds['${k}'] 既没有 data 也没有 dataB64`);
    }
    const r = check(onnb().run(this.id, JSON.stringify(feeds || {})), 'run');
    return (r.outputs || []) as Tensor[];
  }

  /** 只要第 n 个输出（常见用法：模型只关心一个结果） */
  runOne(feeds: Record<string, Feed>, output = 0): Tensor {
    const outs = this.run(feeds);
    const t = outs[output];
    if (!t) throw new OnnxError('invalid', `[onnx] 模型没有第 ${output} 个输出（共 ${outs.length} 个）`);
    return t;
  }

  /** 释放这个会话（模型不再占内存）。之后再用会抛 released。 */
  free(): void {
    if (this.freed) return;
    this.freed = true;
    onnb().release(this.id);
  }

  toString(): string { return `Session#${this.id}<${this.model}>`; }
}

/** 加载模型。失败抛错（错误里带 ORT 的原话，比如"不支持的 IR 版本"）。 */
export function load(path: string, numThreads = 0): Session {
  if (!available())
    throw new OnnxError('notBuilt', '[onnx] 本构建未编入 ONNX Runtime（这个 exe 没接 ORT）');
  if (!path) throw new OnnxError('invalid', '[onnx] 没给模型路径');
  const r = check(onnb().load(path, numThreads), `加载 ${path}`);
  if (typeof r.session !== 'number' || r.session <= 0)
    throw new OnnxError('invalid', '[onnx] 后端没有返回会话句柄');
  return new Session(r.session, path);
}

/** 现在加载着几个模型 */
export function count(): number { return available() ? onnb().count() : 0; }
/** 释放全部会话 */
export function releaseAll(): number { return available() ? onnb().releaseAll() : 0; }

// ————————————————————————————————————————————————————————————
// 小工具（纯 JS；跑分类/检测模型时最常用的两件事）
// ————————————————————————————————————————————————————————————

/** 最大值及其下标（分类模型的 argmax；也用来在概率图里找峰值） */
export function argmax(t: Tensor): { index: number; value: number } {
  let bi = -1, bv = -Infinity;
  for (let i = 0; i < t.data.length; i++) {
    const v = t.data[i];
    if (v > bv) { bv = v; bi = i; }
  }
  return { index: bi, value: bv };
}

/** 前 k 大的 {index,value}，按值降序（检测模型筛候选框用） */
export function topK(t: Tensor, k: number): { index: number; value: number }[] {
  const all = t.data.map((value, index) => ({ index, value }));
  all.sort((a, b) => b.value - a.value);
  return all.slice(0, Math.max(0, k));
}

// ————————————————————————————————————————————————————————————
// default 导出：给 `import onnx from 'onnx'` 用（具名导入是主用法）
// ————————————————————————————————————————————————————————————
const onnx = { OnnxError, Session, available, version, load, count, releaseAll, argmax, topK };
export default onnx;
