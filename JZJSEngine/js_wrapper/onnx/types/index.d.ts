// onnx —— 权威类型定义（运行期实现在 ../index.ts）
//
// 语义与 __jz.onnx（C++ 侧 JZJSOnnxModule）一一对应：
//   · 张量数据走 **number[]**（或 base64 原始字节，大张量用 dataB64）；
//   · 形状里**不接受动态维度**：模型给的 -1 在喂数据时必须换成具体值；
//   · 失败**抛错**（OnnxError），不返回空数组假装成功。

/** 本版支持的张量类型（模型输出可能是别的，那会在 Tensor.note 里说明而不是糊一个空数组） */
export type DType = 'float32' | 'float64' | 'int64' | 'int32' | 'int16' | 'int8' | 'uint16' | 'uint8' | 'bool';
/** 模型声明的类型（可能超出本版能搬的范围） */
export type AnyDType = DType | 'string' | 'unsupported';

/** 模型对某个输入/输出的声明（形状里 -1 = 动态维度） */
export interface TensorSpec {
  name: string;
  dtype: AnyDType;
  shape: number[];
}

/** 喂给模型的一个输入 */
export interface Feed {
  /** 具体形状（动态维度在这里给值，例：[1, 3, 224, 224]）；元素个数必须与之相符 */
  shape: number[];
  /** 默认 'float32' */
  dtype?: DType;
  /** 元素数组 */
  data?: number[];
  /** 原始字节的 base64（大张量走这条，比 data 数组快得多；按 dtype 宽度解析） */
  dataB64?: string;
}

/** 模型吐回来的一个张量 */
export interface Tensor extends TensorSpec {
  data: number[];
  /** 本版搬不回来的类型会带这句说明（data 为空数组） */
  note?: string;
}

/** __jz.onnx 的原子接口（适配层直接用它；一般用本包的 Session 更好用）。
 *  返回 {ok:true,...} 或 {ok:false, error}；load 的第二个参数是线程数（0 = 交给 ORT）。 */
export interface OnnxBackend {
  available(): boolean;
  version(): string;
  load(path: string, numThreads: number): any;
  info(id: number): any;
  run(id: number, feedsJson: string): any;
  release(id: number): boolean;
  releaseAll(): number;
  count(): number;
}
