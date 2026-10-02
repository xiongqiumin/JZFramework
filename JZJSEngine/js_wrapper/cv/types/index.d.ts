// cv —— 权威类型定义（运行期实现在 ../index.ts）
//
// 这一层说的是"什么形状的参数/结果"，语义与 __jz.cv 的 C++ 侧（JZJSCvModule）一一对应：
//   · 码表一律**字符串**，大小写与 CV_/COLOR_/MORPH_ 之类的常量前缀都能省：
//       'BGR2GRAY' / 'bgr2gray' / 'COLOR_BGR2GRAY' 等价（见 JZJSCvModule.cpp 的 normKey）。
//   · 尺寸/点/矩形既收数组也收对象：ksize 可写 5 或 [5,5]；point 可写 [x,y] 或 {x,y}。
//   · 失败会**抛错**（CvError），不会返回空图假装成功。

/** 图像句柄的公共形状（真正的 Mat 类由 ../index.ts 提供） */
export interface MatLike { readonly id: number; }

/** 尺寸：数字（正方形）/ [宽,高] / {w,h} */
export type Size2 = number | readonly [number, number] | { w: number; h: number };
/** 点：[x,y] / {x,y} */
export type Point2 = readonly [number, number] | { x: number; y: number };
/** 矩形：{x,y,w,h} / [x,y,w,h] */
export type RectArg = { x: number; y: number; w: number; h: number } | readonly [number, number, number, number];
/** 颜色：灰度值 / [b,g,r] / [b,g,r,a] / '#RRGGBB'（HTML 写法，内部换成 BGR） */
export type ColorArg = number | readonly number[] | string;
/** 结构元素/卷积核：n（n×n 全 1）/ [[..],[..]] / {data:[[..]]} / {shape,ksize} */
export type KernelArg = number | readonly (readonly number[])[] | { data: number[][] }
                     | { shape: 'rect' | 'ellipse' | 'cross'; ksize: Size2 };

export type MatType = '8UC1' | '8UC3' | '8UC4' | '16UC1' | '32FC1' | '32FC3' | '64FC1' | (string & {});
export type ColorCode = 'BGR2GRAY' | 'RGB2GRAY' | 'GRAY2BGR' | 'BGR2RGB' | 'BGR2HSV' | 'HSV2BGR'
                      | 'BGR2HLS' | 'BGR2LAB' | 'BGR2YUV' | 'BGR2YCrCb' | 'BGRA2BGR' | 'GRAY2BGRA' | (string & {});
export type Interp = 'nearest' | 'linear' | 'cubic' | 'area' | 'lanczos4' | (string & {});
export type BorderType = 'constant' | 'replicate' | 'reflect' | 'reflect101' | 'wrap' | 'default' | (string & {});
export type ThreshType = 'binary' | 'binaryInv' | 'trunc' | 'tozero' | 'tozeroInv' | 'otsu' | 'triangle' | (string & {});
export type MorphOp = 'open' | 'close' | 'gradient' | 'tophat' | 'blackhat' | 'erode' | 'dilate' | (string & {});
export type Colormap = 'jet' | 'hot' | 'bone' | 'rainbow' | 'ocean' | 'summer' | 'viridis' | 'turbo' | (string & {});
export type RetrMode = 'external' | 'list' | 'ccomp' | 'tree' | (string & {});
export type ChainMethod = 'none' | 'simple' | (string & {});
export type TmMethod = 'sqdiff' | 'sqdiffNormed' | 'ccorr' | 'ccorrNormed' | 'ccoeff' | 'ccoeffNormed' | (string & {});
export type NormType = 'inf' | 'l1' | 'l2' | 'l2sqr' | 'hamming' | 'minmax' | (string & {});
export type CmpOp = 'eq' | 'gt' | 'ge' | 'lt' | 'le' | 'ne' | (string & {});
export type FlipHow = 'horizontal' | 'vertical' | 'both' | (string & {});
export type RotateCode = 0 | 1 | 2 | 'ROTATE_90_CLOCKWISE' | 'ROTATE_180' | 'ROTATE_90_COUNTERCLOCKWISE' | (string & {});

/** 图像信息（cv.info / mat.info()） */
export interface MatInfo {
  cols: number;
  rows: number;
  channels: number;
  depth: number;
  type: number;
  /** 如 '8UC3' —— 可直接回传给 zeros/convertTo 的 type */
  typeName: string;
  empty: boolean;
  total: number;
  elemSize: number;
  continuous: boolean;
}

/** 一个轮廓（findContours 的每项：常用派生量都算好了，省得再走一遍） */
export interface Contour {
  points: [number, number][];
  /** 轮廓围出的面积（像素²） */
  area: number;
  /** 周长 */
  length: number;
  /** 4πA/L²：1.0 = 正圆，越小越"毛" —— 用来筛圆球很顺手 */
  circularity: number;
  boundingRect: { x: number; y: number; w: number; h: number };
  /** boundingRect 的中心（常用于"点它"） */
  center: [number, number];
  minAreaRect?: { center: [number, number]; size: [number, number]; angle: number };
  hull: [number, number][];
  hullArea: number;
  /** 多边形近似（顶点数少，便于判"三角形/四边形"） */
  approx: [number, number][];
}

/** 连通域结果 */
export interface ComponentsResult {
  /** 标签图（CV_32S；标签 0 是背景） */
  labels: MatLike;
  count: number;
  /** 每行 [x, y, w, h, area]（第 0 行是背景） */
  stats: number[][];
  /** 每行 [cx, cy]（质心，亚像素） */
  centroids: number[][];
}

export interface MinMaxLocResult { minVal: number; maxVal: number; minLoc: [number, number]; maxLoc: [number, number]; }
export interface MeanStdDevResult { mean: number[]; stddev: number[]; }
export interface MomentsResult {
  m00: number; m10: number; m01: number; m20: number; m11: number; m02: number;
  m30: number; m21: number; m12: number; m03: number;
  nu20: number; nu11: number; nu02: number; nu30: number;
  /** 7 个 Hu 不变矩（旋转/缩放不变，做形状比对用） */
  hu: number[];
}
export interface MatchTemplateResult extends MinMaxLocResult {
  /** 匹配分数图（CV_32F），尺寸 = (W-w+1) × (H-h+1) */
  mat: MatLike;
}
export interface HistResult {
  /** 直方图本身（CV_32F，1×bins） */
  mat: MatLike;
  counts: number[];
}
export interface ThresholdResult {
  mat: MatLike;
  /** 实际用的阈值（OTSU/TRIANGLE 会自己算，这里拿得到） */
  thresh: number;
}

/** __jz.cv 的原子接口（适配层直接用它；一般用本包的函数/Mat 更好用）。
 *  这些是 C++ 侧 JZJSCvModule 注册的东西，失败返回 0/空串/{error}。 */
export interface CvBackend {
  /** 本构建是否编入了 OpenCV（没编的话其余调用都不好使，先问这一句） */
  available(): boolean;
  create(optsJson: string): number;
  info(id: number): MatInfo | { error: string };
  clone(id: number): number;
  release(id: number): boolean;
  releaseAll(): number;
  count(): number;
  imread(path: string): number;
  imwrite(id: number, path: string): boolean;
  imdecode(data: string): number;
  imencode(id: number, ext: string): string;
  data(id: number): string;
  fromData(dataB64: string, optsJson: string): number;
  pixel(id: number, x: number, y: number): number[] | null;
  setPixel(id: number, x: number, y: number, valuesJson: string): boolean;
  /** 通用运算口：{ok:true, mat|mats|value|...} 或 {ok:false, error} */
  op(name: string, paramsJson: string): any;
  /** 支持的操作名清单 */
  ops(): string[];
}
