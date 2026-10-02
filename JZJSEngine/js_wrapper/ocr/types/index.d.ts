// ocr —— 权威类型定义（运行期实现在 ../index.ts）
//
// 语义与 __jz.computeruse 的 OCR 那一组（C++ 侧 JZComputerUse::initOcr/ocrTexts/...）一一对应：
//   · 矩形都是**归一化**的（相对整块屏幕/虚拟桌面，[0,1]），与 layoutTree / click 同一口径
//     ⇒ 拿到的 rect 可以直接喂给 computeruse 的 click；
//   · "有没有 OCR 能力"(built) 与 "模型加载好没"(ready) 是**两件事**，别混；
//   · 没编入 / 没 init / 没识别到，一律返回空数组或 null —— 不会假装成功。

/** 归一化矩形（相对整屏/整块虚拟桌面） */
export interface Rect { x: number; y: number; w: number; h: number }

/** 识别到的一段文字 */
export interface TextHit {
  text: string;
  /** 识别置信度（后端默认只给 >= minScore 的） */
  score: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

/** 诊断信息（`info()`）：ready=false 时 modelRoot 往往就是问题所在 */
export interface OcrInfo {
  /** 本构建是否**编入了** OCR（JZ_CU_OCR：要 JZCv = OpenCV + ONNX Runtime） */
  built: boolean;
  /** 模型是否已加载（init() 成功过） */
  ready: boolean;
  /** 是否带方向分类模型（PP-OCR 的 cls；本机这套没有） */
  cls: boolean;
  /** 实际用上的模型目录（空 = 还没 init 到） */
  modelRoot: string;
  /** 没编入时的一句说明 */
  hint?: string;
}

/** find / findAll 的匹配选项 */
export interface FindOptions {
  /** 只在某个区域内找（归一化） */
  region?: Rect;
  /** 识别置信度下限，默认 0.6 */
  minScore?: number;
  /** true = 整段完全相等（默认是"包含"，都不区分大小写） */
  exact?: boolean;
  /** true = 把 text 当正则（或直接给一个 RegExp） */
  regex?: boolean | RegExp;
}

/** texts() 的选项 */
export interface TextsOptions {
  region?: Rect;
  minScore?: number;
}

/** __jz.computeruse 里 OCR 那一组的形状（适配层直接用它；一般用本包的函数更顺手） */
export interface OcrBackend {
  initOcr(optsJson: string): boolean;
  ocrReady(): boolean;
  ocrTexts(optsJson: string): TextHit[];
  ocrBuilt?(): boolean;          // 老宿主可能没有
  ocrInfo?(): OcrInfo;
  deinitOcr?(): void;
}
