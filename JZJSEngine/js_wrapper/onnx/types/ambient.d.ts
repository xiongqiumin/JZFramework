// 'onnx' —— 对外的裸模块名(= 目录名): 只声明名字 ——
//   · **类型** <- ./index  = onnx/types/index.d.ts (本包权威类型定义)
//   · **值**   <- ../index = onnx/index.ts        (运行期实现, 编译成 dist/index.js)
//
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './x'` 不转发**具名导出**
//   (只转发 default), 故一律**显式具名再导出**(与 computeruse / cv 包同一个坑、同一个写法)。
//   ⛔ 本包加新导出时, 必须同时在下面两张表里补名, 否则 `import { X } from 'onnx'` 报 TS2305。
declare module 'onnx' {
    export type { DType, AnyDType, TensorSpec, Feed, Tensor, OnnxBackend } from './index';
    export { OnnxError, Session, available, version, load, count, releaseAll, argmax, topK } from '../index';
    export { default } from '../index';
}
