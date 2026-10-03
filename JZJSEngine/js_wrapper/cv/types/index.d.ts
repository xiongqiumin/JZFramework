// cv —— 权威类型定义（运行期实现在 ../index.ts）
//
// 这一层说的是"什么形状的参数/结果"，语义与 __jz.cv 的 C++ 侧（JZJSCvModule）一一对应：
//   · 码表一律**字符串**，大小写与 CV_/COLOR_/MORPH_ 之类的常量前缀都能省：
//       'BGR2GRAY' / 'bgr2gray' / 'COLOR_BGR2GRAY' 等价（见 JZJSCvModule.cpp 的 normKey）。
//   · 尺寸/点/矩形既收数组也收对象：ksize 可写 5 或 [5,5]；point 可写 [x,y] 或 {x,y}。
//   · 失败会**抛错**（CvError），不会返回空图假装成功。

/** 图像句柄的公共形状（真正的 Mat 类由 ../index.ts 提供） */
export interface MatLike { readonly id: number; }

/** 一个 **JZImage**（widget 包的全局类；例如 `computeruse.screenshotImage()` 给的那一帧）。
 *  本包刻意**不 import** widget 的类型：cv 是自包含包，"只装了 cv"的工程也得能解析。
 *  所以这里只声明要用到的那三个方法 —— 运行期由 C++ 侧按**真实类型**校验（传普通对象会抛）。
 *  用法：`const m = cv.fromImage(img); cv.cvtColor(m, cv.COLOR_RGBA2BGR)` */
export interface JZImageLike {
  width(): number;
  height(): number;
  isNull(): boolean;
}

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

export type MatType = '8UC1' | '8UC3' | '8UC4' | '16UC1' | '32FC1' | '32FC3' | '64FC1' | (string & {}) | number;
export type ColorCode = 'BGR2GRAY' | 'RGB2GRAY' | 'GRAY2BGR' | 'BGR2RGB' | 'BGR2HSV' | 'HSV2BGR'
                      | 'BGR2HLS' | 'BGR2LAB' | 'BGR2YUV' | 'BGR2YCrCb' | 'BGRA2BGR' | 'GRAY2BGRA' | (string & {}) | number;
export type Interp = 'nearest' | 'linear' | 'cubic' | 'area' | 'lanczos4' | (string & {}) | number;
export type BorderType = 'constant' | 'replicate' | 'reflect' | 'reflect101' | 'wrap' | 'default' | (string & {}) | number;
export type ThreshType = 'binary' | 'binaryInv' | 'trunc' | 'tozero' | 'tozeroInv' | 'otsu' | 'triangle' | (string & {}) | number;
export type MorphOp = 'open' | 'close' | 'gradient' | 'tophat' | 'blackhat' | 'erode' | 'dilate' | (string & {}) | number;
export type Colormap = 'jet' | 'hot' | 'bone' | 'rainbow' | 'ocean' | 'summer' | 'viridis' | 'turbo' | (string & {}) | number;
export type RetrMode = 'external' | 'list' | 'ccomp' | 'tree' | (string & {}) | number;
export type ChainMethod = 'none' | 'simple' | (string & {}) | number;
export type TmMethod = 'sqdiff' | 'sqdiffNormed' | 'ccorr' | 'ccorrNormed' | 'ccoeff' | 'ccoeffNormed' | (string & {}) | number;
export type NormType = 'inf' | 'l1' | 'l2' | 'l2sqr' | 'hamming' | 'minmax' | (string & {}) | number;
export type CmpOp = 'eq' | 'gt' | 'ge' | 'lt' | 'le' | 'ne' | (string & {}) | number;
export type FlipHow = 'horizontal' | 'vertical' | 'both' | (string & {}) | number;
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
  /** 与 OpenCV.js 对齐的空操作：轮廓是普通 JS 对象，不占句柄，写了不报错 */
  delete(): void;
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

// —— 传统算法补充(2026-10-03)的结构化结果 ——
/** 直角 ↔ 极坐标互转的结果 */
export interface CartToPolarResult { magnitude: MatLike; angle: MatLike; }
export interface PolarToCartResult { x: MatLike; y: MatLike; }
/** 积分图（前缀和）：sum 必有；sqsum / tilted 按需 */
export interface IntegralResult { sum: MatLike; sqsum?: MatLike; tilted?: MatLike; }
/** 距离变换：dist 是距离图（CV_32F）；labels 仅在 labels=true 时有 */
export interface DistanceTransformResult { dist: MatLike; labels?: MatLike; }
/** 漫水填充：填充后的**新图** + 填充像素数 + 外接矩形 */
export interface FloodFillResult {
  mat: MatLike; count: number;
  rect: { x: number; y: number; w: number; h: number };
  /** OpenCV 的填充掩膜（(rows+2)×(cols+2) 的 8U：填过的点 = 1，边界 = 2）；没要就是 null */
  mask: MatLike | null;
}
/** K 均值：每个点的标签 / 每个簇中心 / 紧凑度（越小越紧） */
export interface KmeansResult { labels: MatLike; centers: MatLike; compactness: number; }
export interface MinEnclosingCircleResult { center: { x: number; y: number }; radius: number; }
export interface FitEllipseResult { center: { x: number; y: number }; size: { width: number; height: number }; angle: number; }
/** 单应矩阵 + 内点掩膜（仅 ransac/lmeds/rho 会给） */
export interface FindHomographyResult { mat: MatLike; mask: MatLike | null; }
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
  /** 把一个 JZImage 的像素**拷**成新 Mat(8UC4 RGBA) -> id（0 = 空图 / 给的不是 JZImage） */
  fromImage(img: JZImageLike): number;
  pixel(id: number, x: number, y: number): number[] | null;
  setPixel(id: number, x: number, y: number, valuesJson: string): boolean;
  /** 通用运算口：{ok:true, mat|mats|value|...} 或 {ok:false, error} */
  op(name: string, paramsJson: string): any;
  /** 支持的操作名清单 */
  ops(): string[];
}

// ==================== 以下为**生成段**（勿手改, 见 gen_types.py） ====================
/** 尺寸（= OpenCV.js 的 cv.Size）。注意宽度在前, 与 cv::Size 一致（width = cols）。 */
export declare class Size {
    width: number;
    height: number;
    constructor(width: number, height: number);
}
/** 点（= cv.Point）。⚠ 同时支持两种读法: `p.x / p.y`（OpenCV.js 的形状）与 `p[0] / p[1]`
 *  （我们内部的简写）—— 于是 `minMaxLoc(...).maxLoc` 这类返回值两边都写得通。 */
export declare class Point {
    x: number;
    y: number;
    [index: number]: number;
    constructor(x: number, y: number);
}
/** 矩形（= cv.Rect）。x/y/width/height 同 OpenCV.js；w/h 是别名（我们自己的简写里用 w/h）。 */
export declare class Rect {
    x: number;
    y: number;
    width: number;
    height: number;
    constructor(x: number, y: number, width: number, height: number);
    get w(): number;
    get h(): number;
}
/** 颜色/多通道值（= cv.Scalar）。像数组一样用：`new Scalar(0,0,255)`、`s[0]`、`s.length`。 */
export declare class Scalar {
    [index: number]: number;
    length: number;
    constructor(...values: number[]);
}
/** 圆（= cv.Circle）。我们自己的 circle() 直接收 center + radius, 不必非包一层。 */
export declare class Circle {
    center: Point;
    radius: number;
    constructor(center: Point, radius: number);
}
/** 迭代算法的终止条件（= cv.TermCriteria）—— 目前只有 kmeans 用得上。
 *  OpenCV.js 写法：`new cv.TermCriteria(cv.TERM_CRITERIA_EPS + cv.TERM_CRITERIA_MAX_ITER, 10, 1.0)` */
export declare class TermCriteria {
    type: number;
    maxCount: number;
    epsilon: number;
    constructor(type?: number, maxCount?: number, epsilon?: number);
}
/** 带角度的矩形（= cv.RotatedRect）。angle 与 OpenCV 同义（度, 逆时针为正）。 */
export declare class RotatedRect {
    center: Point;
    size: Size;
    angle: number;
    constructor(center: Point, size: Size, angle: number);
}
/** 是不是本包的 Mat 句柄 —— 用**鸭子类型**判(不 import Mat, 免得与 index.ts 循环)。 */
export declare function isMatLike(v: any): boolean;
/** 把值对象/简写统一成后端认的形状：Size->{w,h}，Rect->{x,y,w,h}，Point->[x,y]，
 *  Scalar->数字数组，Mat->句柄 id；数组/普通对象递归处理。 */
export declare function normArg(v: any): any;

/** cv 的错误：`code` 便于调用方按类处理，不靠错误文本判断。 */
export declare class CvError extends Error {
    code: 'noBackend' | 'notBuilt' | 'failed' | 'invalid' | 'released';
    constructor(code: CvError['code'], message: string);
}
/** 本构建是否编入了 OpenCV。没编时其余调用都会抛 notBuilt —— 先问这一句再决定走哪条路。 */
export declare function available(): boolean;
export declare class Mat {
    /** 句柄 id（C++ 侧那张表的键）。**0 = 还没绑图像**（OpenCV.js 里 `new cv.Mat()` 就是这个状态，
     *  专门当 dst 出参的占位）；不要把 id 当业务数据用。 */
    id: number;
    private freed;
    private bytesCache;
    /** 三种构造方式（= OpenCV.js 那套，也保留我们的"包句柄"用法）：
     *   · `new Mat()`                        —— 空图，等着被当 dst 写进来（`cvtColor(src, dst, code)`）
     *   · `new Mat(rows, cols, type[, fill])` —— OpenCV.js 写法：直接建一张
     *   · `new Mat(handleId)`                —— 内部：包一个已有句柄（本包的函数用） */
    constructor(rowsOrId?: number, cols?: number, type?: MatType | number, fill?: ColorArg);
    /** OpenCV.js 的 `cv.Mat.zeros(rows, cols, type)` */
    static zeros(rows: number, cols: number, type?: MatType | number): Mat;
    /** OpenCV.js 的 `cv.Mat.ones(rows, cols, type)` */
    static ones(rows: number, cols: number, type?: MatType | number): Mat;
    /** OpenCV.js 的 `cv.Mat.eye(rows, cols, type)` —— 单位矩阵（后端没有这个 op, 这里在 JS 里拼好再送进去） */
    static eye(rows: number, cols: number, type?: MatType | number): Mat;
    /** @internal dst 出参语义：把 another 的句柄收到自己身上（自己原来那张先放掉）。
     *  后端每个 op 都返回**新句柄**，所以"写进 dst" = 换绑句柄；dst 这个 JS 对象的身份不变。 */
    __bind(another: Mat): void;
    private release;
    /** 图像信息（宽/高/通道/类型…） */
    get info(): MatInfo;
    get cols(): number;
    get rows(): number;
    /** 通道数（OpenCV.js：`mat.channels()` —— 是**方法**） */
    channels(): number;
    /** 深度：CV_8U=0 / CV_8S=1 / CV_16U=2 / CV_16S=3 / CV_32S=4 / CV_32F=5 / CV_64F=6（方法） */
    depth(): number;
    /** OpenCV 的整型类型（CV_8UC3 = 16 …，方法）：可直接回传给 zeros/convertTo/Mat 构造 */
    type(): number;
    /** 类型名（**本包扩展**：OpenCV.js 没有这一项） */
    get typeName(): string;
    /** 空图？（方法） */
    empty(): boolean;
    /** 像素总数（不含通道，方法） */
    total(): number;
    isContinuous(): boolean;
    /** 每像素字节数 / 每通道字节数（方法） */
    elemSize(): number;
    elemSize1(): number;
    /** 行字节数（OpenCV.js 里 `step` 是**属性**） */
    get step(): number;
    /** 每通道的行字节数（方法） */
    step1(): number;
    /** [rows, cols]（OpenCV.js 里 `matSize` 是**属性**） */
    get matSize(): [number, number];
    /** 尺寸对象（OpenCV.js 的 `mat.size()` 回 cv.Size） */
    size(): Size;
    /** 深拷贝一份（之后可以各自 free） */
    clone(): Mat;
    /** 释放这个句柄（C++ 侧那份像素也就没了）。之后再用会抛 released。 */
    free(): void;
    /** OpenCV.js 的写法：`mat.delete()`（与 free() 同义；对 `new Mat()` 的空图是空操作） */
    delete(): void;
    /** OpenCV.js 的 `mat.isDeleted()` */
    isDeleted(): boolean;
    /** 取一个像素：[b,g,r] / [gray] … （类型不同长度不同）；越界返回 null */
    pixel(x: number, y: number): number[] | null;
    /** 写一个像素（改的是这张图本身；少量采样/打点用，批量请用掩膜运算） */
    setPixel(x: number, y: number, values: number[] | number): boolean;
    /** 原始字节的 base64（8U；非 8U 会先转 8U）—— 存下来/传给 fromData() 用 */
    /** 原始字节的 base64（按真实类型；配合 `fromData(b64, {rows, cols, type})` 能原样还原） */
    dataB64(): string;
    private bytes;
    /** 原始字节按指定 Ctor 重新解释（= OpenCV.js 的 data32F/data64F… 那组视图） */
    private typed;
    /** 原始字节（**快照**，按真实类型排布：32F 就是 4 字节/通道的 float 位模式）。
     *  ⚠ OpenCV.js 的 mat.data 是 WASM 堆视图、改了就生效；我们后端在 C++ 侧, 只能给一份拷贝 ——
     *  改它**不会**写回图像, 要写回请用 fromData() / setPixel()。 */
    get data(): Uint8Array;
    get data8U(): Uint8Array;
    get data8S(): Int8Array;
    get data16U(): Uint16Array;
    get data16S(): Int16Array;
    get data32S(): Int32Array;
    get data32F(): Float32Array;
    get data64F(): Float64Array;
    /** 第 (row,col) 个像素的第 0 个通道（OpenCV.js 的 ucharAt/floatAt/intAt/doubleAt 同理） */
    ucharAt(row: number, col: number): number;
    floatAt(row: number, col: number): number;
    intAt(row: number, col: number): number;
    doubleAt(row: number, col: number): number;
    charAt(row: number, col: number): number;
    shortAt(row: number, col: number): number;
    ushortAt(row: number, col: number): number;
    /** 像素"指针"（OpenCV.js 里 `p[0]` 能读能写）。⚠ 我们每次读写都走 pixel/setPixel, 不是真指针 */
    private ptrAt;
    ucharPtr(row: number, col?: number): any;
    floatPtr(row: number, col?: number): any;
    intPtr(row: number, col?: number): any;
    doublePtr(row: number, col?: number): any;
    ptr(row: number, col?: number): any;
    charPtr(row: number, col?: number): any;
    shortPtr(row: number, col?: number): any;
    ushortPtr(row: number, col?: number): any;
    /** 取一块子图（⚠ 我们这边是**拷贝**: 改写子图不影响原图, 与 OpenCV.js 的视图语义不同） */
    roi(rect: RectArg): Mat;
    /** 第 i 行（拷贝） */
    row(i: number): Mat;
    /** 第 i 列（拷贝） */
    col(i: number): Mat;
    rowRange(start: number, end: number): Mat;
    colRange(start: number, end: number): Mat;
    /** 把内容拷进 dst（dst 的身份保留；OpenCV.js：`src.copyTo(dst)` 无返回） */
    copyTo(dst: Mat): void;
    /** 转类型/缩放。OpenCV.js 形状：`src.convertTo(dst, rtype[, alpha, beta])` —— 写进 dst, **无返回**；
     *  不给 dst 时（本包扩展）返回新图。 */
    convertTo(dst: Mat, rtype?: MatType, alpha?: number, beta?: number): void;
    convertTo(rtype?: MatType, alpha?: number, beta?: number): Mat;
    /** OpenCV.js 的 `mat.t()`：转置 —— 回**新 Mat**（与 OpenCV.js 一致, 不改自己） */
    t(): Mat;
    /** OpenCV.js 的 `mat.mul(m[, scale])`：逐元素相乘 —— `A.mul(B, k)` = A∘B·k（尺寸类型要一致）→ 新 Mat */
    mul(m: Mat, scale?: number): Mat;
    /** OpenCV.js 的 `mat.dot(m)`：点积（尺寸/类型必须一致, 按行优先当向量看）→ number */
    dot(m: Mat): number;
    /** OpenCV.js 的 `mat.inv([method])`：矩阵求逆（默认 DECOMP_LU；奇异时 OpenCV 给全 0 阵, 与 C++ 一致） */
    inv(method?: number): Mat;
    /** OpenCV.js 的 `mat.diag([d])`：抽一条对角线 —— d=0 主对角, d>0 主对角**下方**, d<0 **上方** → 新 Mat */
    diag(d?: number): Mat;
    /** 整张填成某个值（⚠ 不支持 mask, 与 OpenCV.js 的 setTo(value, mask) 不同） */
    setTo(value: ColorArg): void;
    /** 重新分配成 rows×cols×type（与 OpenCV.js 一样是"够用就不动"的近似: 这里一律重分配） */
    create(rows: number, cols: number, type: MatType | number): void;
    /** 存到磁盘（按扩展名定格式：.png/.jpg/.bmp/...） */
    save(path: string): boolean;
    /** 编码成 dataURL（默认为 PNG）—— 可直接交给需要图片的地方 */
    encode(ext?: string): string;
    toString(): string;
}
/** 全 0 图像 */
export declare function zeros(cols: number, rows: number, type?: MatType): Mat;
/** 全 1 图像 */
export declare function ones(cols: number, rows: number, type?: MatType): Mat;
/** 用某个颜色填满（例：白色底 full(100, 100, '8UC3', 255)） */
export declare function full(cols: number, rows: number, type: MatType, fill: ColorArg): Mat;
/** 从原始像素字节恢复一张图（b64 来自 mat.dataB64()） */
export declare function fromData(dataB64: string, opts: {
    cols: number;
    rows: number;
    type?: MatType;
}): Mat;
/** 把一个 **JZImage**（全局类；例如 `computeruse.screenshotImage()` 那一帧）的像素**拷进**一张新 Mat。
 *  产出 **8UC4**，通道序**保持原样 (R,G,B,A)** —— 本函数只"搬"、不"转"：
 *    要 BGR / 灰度就接着 `cvtColor(m, cv.COLOR_RGBA2BGR / cv.COLOR_RGBA2GRAY)`。
 *  为什么不用 `imdecode(await cu.screenshot())`：帧本来就在内存里，dataURL 那条路要
 *    "PNG 编码 + 再解码"两遍（帧越大越贵），而这里只是 memcpy。
 *  ⚠ 是**拷贝**、不是零拷贝视图：拿到的 Mat 与那个 JZImage 再无关系，不必操心谁先释放。 */
export declare function fromImage(img: JZImageLike): Mat;
/** 从 base64 / dataURL（PNG/JPEG/...）解出一张图 —— 容器里拿截图的第一站 */
export declare function imdecode(data: string): Mat;
/** 现在挂着几个句柄（看看有没有忘了 free） */
export declare function count(): number;
/** 释放全部句柄（一轮处理结束时调一次最省心） */
export declare function releaseAll(): number;
/** 支持的操作名（自检/探活用） */
export declare function ops(): string[];
/** 通用出口：op('gaussianBlur', { src: m.id, ksize: [5,5] }) —— 新加的 C++ 侧能力不用等本包更新 */
export declare function op(name: string, params: Record<string, unknown>): any;
/** 类型/线性变换：dst = src * alpha + beta（改深度/缩放范围用） */
export declare function convertTo(src: Mat, type: MatType, alpha?: number, beta?: number): Mat;
/** 查表映射（256 项起，如提亮/二值化/伪彩） */
export declare function lut(src: Mat, table: number[]): Mat;
/** 自适应直方图均衡（CLAHE；局部对比度，比全局均匀化更适合"暗角落里的目标"） */
export declare function clahe(src: Mat, clipLimit?: number, tileGridSize?: Size2): Mat;
/** 高斯滤波（去噪保边最常用） */
export declare function gaussianBlur(src: Mat, ksize?: Size2, sigmaX?: number, opts?: {
    sigmaY?: number;
    borderType?: BorderType;
}): Mat;
/** Sobel 梯度（dx/dy 要哪个方向给 1） */
export declare function sobel(src: Mat, dx?: number, dy?: number, ksize?: number, opts?: {
    ddepth?: MatType;
    scale?: number;
    delta?: number;
}): Mat;
export declare function scharr(src: Mat, dx?: number, dy?: number, opts?: {
    ddepth?: MatType;
    scale?: number;
}): Mat;
export declare function laplacian(src: Mat, ksize?: number, opts?: {
    ddepth?: MatType;
    scale?: number;
}): Mat;
/** Canny 边缘（多通道会先转灰度） */
export declare function canny(src: Mat, threshold1?: number, threshold2?: number, opts?: {
    apertureSize?: number;
    l2gradient?: boolean;
}): Mat;
/** 裁剪（越界会自动裁到图内） */
export declare function crop(src: Mat, rect: RectArg): Mat;
export declare function crop(src: Mat, x: number, y: number, w: number, h: number): Mat;
/** 旋转 90/180/270（默认顺时针 90） */
export declare function rotate(src: Mat, code?: RotateCode): Mat;
/** 转置（行列互换） */
export declare function transpose(src: Mat): Mat;
/** 绕某点旋转+缩放的 2×3 矩阵 */
export declare function getRotationMatrix2D(center: Point2, angle: number, scale?: number): Mat;
/** 4 点 -> 4 点的 3×3 透视矩阵（矫正斜拍/梯形） */
export declare function getPerspectiveTransform(srcPts: Point2[], dstPts: Point2[]): Mat;
/** 横向拼接 */
export declare function hconcat(mats: Mat[]): Mat;
/** 纵向拼接 */
export declare function vconcat(mats: Mat[]): Mat;
export declare function bitwiseAnd(a: Mat, b: Mat): Mat;
export declare function bitwiseOr(a: Mat, b: Mat): Mat;
export declare function bitwiseXor(a: Mat, b: Mat): Mat;
/** 取反（掩膜反相很方便） */
export declare function bitwiseNot(src: Mat): Mat;
/** 非零像素数（掩膜"有多大"）。
 *  ⚠ 只吃**单通道**（OpenCV 的约束）：彩色图先 `cvtColor(img, 'BGR2GRAY')`，否则后端会抛错。 */
export declare function countNonZero(src: Mat): number;
/** 逐通道求和 */
export declare function sum(src: Mat): number[];
/** OpenCV.js 的 `cv.mean(src[, mask])` -> Scalar（各通道均值, 不足 4 个补 0） */
export declare function mean(src: Mat, mask?: Mat): Scalar;
/** 范数（两图相似度：norm(a,b,'l2') 越小越像） */
export declare function norm(a: Mat, b?: Mat, normType?: NormType): number;
/** 找轮廓（**OpenCV.js 形状**）：结果填进 `contours`（MatVector，每项是**轮廓 Mat** 的句柄 ——
 *  点活在 C++ 里, 一个点都不序列化）, 层次结构填进 `hierarchy`（Mat, Nx1 CV_32SC4）。
 *  派生量按需再问：contourArea / boundingRect / convexHull / approxPolyDP ...（与 OpenCV.js 一样）。
 *  实测比"一次把派生量全回"的 findContours 快一个数量级（那版 65 条轮廓要 69KB JSON）。 */
export declare function findContoursTo(src: Mat, contours: MatVector, hierarchy?: Mat, mode?: RetrMode, method?: ChainMethod): void;
/** 图像矩（含 7 个 Hu 不变矩） */
export declare function moments(src: Mat, binary?: boolean): MomentsResult;
/** 一维直方图（counts 直接可用；mat 是 1×bins 的 CV_32F，可 normalize + 画出来） */
export declare function calcHist(src: Mat, opts?: {
    channel?: number;
    bins?: number;
    min?: number;
    max?: number;
}): HistResult;
/** OpenCV 4.x 的 CV_MAKETYPE：type = depth + ((cn-1) << 3)。⚠ 5.x 换成 5 位通道, 我们跟 4.x。 */
export declare function CV_MAKETYPE(depth: number, cn: number): number;
export declare function CV_8UC(n: number): number;
export declare function CV_8SC(n: number): number;
export declare function CV_16UC(n: number): number;
export declare function CV_16SC(n: number): number;
export declare function CV_32SC(n: number): number;
export declare function CV_32FC(n: number): number;
export declare function CV_64FC(n: number): number;
/** 由 JS 数组造图（OpenCV.js 的 cv.matFromArray）：数组按**通道顺序平铺**（与 OpenCV 一致）。
 *  例：matFromArray(2, 2, CV_8UC3, [0,0,255, 0,255,0, 255,0,0, 0,0,0]) */
export declare function matFromArray(rows: number, cols: number, type: MatType | number, array: ArrayLike<number>): Mat;
/** 由 ImageData({data, width, height}) 造图（OpenCV.js 的 cv.matFromImageData）—— 8UC4(RGBA) */
export declare function matFromImageData(imageData: {
    data: ArrayLike<number>;
    width: number;
    height: number;
}): Mat;
/** cv.MatVector：装一串 Mat / 轮廓（OpenCV.js 里是 std::vector<Mat>）。
 *  我们这边 findContours 给的是**轮廓对象**（派生量都算好了），所以两种都装。 */
export declare class MatVector {
    private items;
    push_back(m: any): void;
    size(): number;
    get(i: number): any;
    set(i: number, m: any): void;
    resize(n: number, m?: any): void;
    /** ⚠ 只清空这张"向量表", 不替调用方释放里面的句柄（各自的 Mat.delete() / releaseAll() 说了算） */
    delete(): void;
}
/** 可以是：我们的 Contour、点数组 [[x,y],...]、或一张 Mat（2 通道 32S/32F） */
export type ContourLike = Contour | ReadonlyArray<readonly [number, number]> | Mat;
/** 取轮廓的点集。⚠ 轮廓是 Mat(句柄) 时这会把点**序列化成 JSON** —— 只在真要"看/算几何"时用;
 *  面积/周长/包围盒这类都走 contourStat（只回一个数, 不搬点）。 */
export declare function contourPoints(c: ContourLike): [number, number][];
/** 轮廓面积 */
export declare function contourArea(c: ContourLike): number;
/** 轮廓周长（closed=false 时是 cv::arcLength 的非闭合算法） */
export declare function arcLength(c: ContourLike, closed?: boolean): number;
/** 正矩形包围盒（回 OpenCV.js 形状的 Rect：x/y/width/height） */
export declare function boundingRect(c: ContourLike): Rect;
/** 最小面积矩形（回 RotatedRect；点少于 5 个时返回 null） */
export declare function minAreaRect(c: ContourLike): RotatedRect | null;
/** 轮廓点数（扩展：省得为了"有几个点"把整条点集搬回来） */
export declare function contourPointCount(c: ContourLike): number;
/** 圆度 4πA/L²（1.0 = 正圆；扩展：富形态里有现成的） */
export declare function contourCircularity(c: ContourLike): number;
/** 凸包（OpenCV.js 的 cv.convexHull(cnt, hull[, clockwise, returnPoints])）—— 回一张 Nx1 的
 *  轮廓 Mat（32SC2 = 点；returnPoints=false 时是 32SC1 = 下标）。 */
export declare function convexHullOf(c: ContourLike, clockwise?: boolean, returnPoints?: boolean): Mat;
/** 多边形近似（OpenCV.js 的 cv.approxPolyDP(cnt, approx, eps, closed)），eps 是**绝对**阈值
 *  —— 回一张 Nx1 的 32SC2 轮廓 Mat。 */
export declare function approxPolyDPOf(c: ContourLike, eps?: number, closed?: boolean): Mat;
/** 轮廓是不是凸的（真 OpenCV 的 cv::isContourConvex） */
export declare function isContourConvex(c: ContourLike): boolean;
/** cv.CLAHE（限制对比度的自适应直方图均衡）：`new CLAHE(clip, new Size(8,8)).apply(src, dst)` */
export declare class CLAHE {
    private clip;
    private tiles;
    constructor(clipLimit?: number, tileGridSize?: Size2);
    apply(src: Mat, dst?: Mat): Mat;
}
/** OpenCV.js 的 cv.createCLAHE(clipLimit, tileGridSize) */
export declare function createCLAHE(clipLimit?: number, tileGridSize?: Size2): CLAHE;
/** 存盘（OpenCV.js 的 cv.imwrite(filename, img) —— ⚠ 参数顺序是**文件名在前**） */
export declare function imwrite(filename: string, img: Mat): boolean;
/** 三点仿射矩阵（2×3 CV_64F）—— 给 warpAffine 用 */
export declare function getAffineTransform(srcTri: Point2[], dstTri: Point2[]): Mat;
/** 霍夫圆检测：回 1×N CV_32FC3 的 (x, y, r)（没找到就是空图） */
export declare function houghCircles(src: Mat, method?: number | string, dp?: number, minDist?: number, param1?: number, param2?: number, minRadius?: number, maxRadius?: number): Mat;
/** 概率霍夫线段检测：回 N×1 CV_32SC4 的 (x1, y1, x2, y2) —— 表格线/边框 */
export declare function houghLinesP(src: Mat, rho?: number, theta?: number, threshold?: number, minLineLength?: number, maxLineGap?: number): Mat;
/** 形状匹配（Hu 矩）：0 = 完全一样；method = i1|i2|i3 */
export declare function matchShapes(a: ContourLike, b: ContourLike, method?: number | string, parameter?: number): number;
/** 直方图比对：correl（1 最像）/ chisqr / intersect（越大越像）/ bhattacharyya·hellinger（越小越像）/ kl */
export declare function compareHist(a: Mat, b: Mat, method?: number | string): number;
/** ⚠ photo 模块未编入：调用会抛错（见上面说明） */
export declare function fastNlMeansDenoising(src: Mat, h?: number, templateWindowSize?: number, searchWindowSize?: number): Mat;
/** ⚠ photo 模块未编入：调用会抛错（见上面说明） */
export declare function inpaint(src: Mat, mask: Mat, inpaintRadius?: number, flags?: number | string): Mat;

export declare const CV_8U = 0;
export declare const CV_8S = 1;
export declare const CV_16U = 2;
export declare const CV_16S = 3;
export declare const CV_32S = 4;
export declare const CV_32F = 5;
export declare const CV_64F = 6;
export declare const CV_8UC1 = 0;
export declare const CV_8UC2 = 8;
export declare const CV_8UC3 = 16;
export declare const CV_8UC4 = 24;
export declare const CV_8SC1 = 1;
export declare const CV_8SC2 = 9;
export declare const CV_8SC3 = 17;
export declare const CV_8SC4 = 25;
export declare const CV_16UC1 = 2;
export declare const CV_16UC2 = 10;
export declare const CV_16UC3 = 18;
export declare const CV_16UC4 = 26;
export declare const CV_16SC1 = 3;
export declare const CV_16SC2 = 11;
export declare const CV_16SC3 = 19;
export declare const CV_16SC4 = 27;
export declare const CV_32SC1 = 4;
export declare const CV_32SC2 = 12;
export declare const CV_32SC3 = 20;
export declare const CV_32SC4 = 28;
export declare const CV_32FC1 = 5;
export declare const CV_32FC2 = 13;
export declare const CV_32FC3 = 21;
export declare const CV_32FC4 = 29;
export declare const CV_64FC1 = 6;
export declare const CV_64FC2 = 14;
export declare const CV_64FC3 = 22;
export declare const CV_64FC4 = 30;
export declare const CV_16F = 7;
export declare const COLOR_BGR2BGR555 = 22;
export declare const COLOR_BGR2BGR565 = 12;
export declare const COLOR_BGR2BGRA = 0;
export declare const COLOR_BGR2GRAY = 6;
export declare const COLOR_BGR2HLS = 52;
export declare const COLOR_BGR2HLS_FULL = 68;
export declare const COLOR_BGR2HSV = 40;
export declare const COLOR_BGR2HSV_FULL = 66;
export declare const COLOR_BGR2Lab = 44;
export declare const COLOR_BGR2Luv = 50;
export declare const COLOR_BGR2RGB = 4;
export declare const COLOR_BGR2RGBA = 2;
export declare const COLOR_BGR2XYZ = 32;
export declare const COLOR_BGR2YCrCb = 36;
export declare const COLOR_BGR2YUV = 82;
export declare const COLOR_BGR2YUV_I420 = 128;
export declare const COLOR_BGR2YUV_IYUV = 128;
export declare const COLOR_BGR2YUV_UYNV = 144;
export declare const COLOR_BGR2YUV_UYVY = 144;
export declare const COLOR_BGR2YUV_Y422 = 144;
export declare const COLOR_BGR2YUV_YUNV = 148;
export declare const COLOR_BGR2YUV_YUY2 = 148;
export declare const COLOR_BGR2YUV_YUYV = 148;
export declare const COLOR_BGR2YUV_YV12 = 132;
export declare const COLOR_BGR2YUV_YVYU = 150;
export declare const COLOR_BGR5552BGR = 24;
export declare const COLOR_BGR5552BGRA = 28;
export declare const COLOR_BGR5552GRAY = 31;
export declare const COLOR_BGR5552RGB = 25;
export declare const COLOR_BGR5552RGBA = 29;
export declare const COLOR_BGR5652BGR = 14;
export declare const COLOR_BGR5652BGRA = 18;
export declare const COLOR_BGR5652GRAY = 21;
export declare const COLOR_BGR5652RGB = 15;
export declare const COLOR_BGR5652RGBA = 19;
export declare const COLOR_BGRA2BGR = 1;
export declare const COLOR_BGRA2BGR555 = 26;
export declare const COLOR_BGRA2BGR565 = 16;
export declare const COLOR_BGRA2GRAY = 10;
export declare const COLOR_BGRA2RGB = 3;
export declare const COLOR_BGRA2RGBA = 5;
export declare const COLOR_BGRA2YUV_I420 = 130;
export declare const COLOR_BGRA2YUV_IYUV = 130;
export declare const COLOR_BGRA2YUV_UYNV = 146;
export declare const COLOR_BGRA2YUV_UYVY = 146;
export declare const COLOR_BGRA2YUV_Y422 = 146;
export declare const COLOR_BGRA2YUV_YUNV = 152;
export declare const COLOR_BGRA2YUV_YUY2 = 152;
export declare const COLOR_BGRA2YUV_YUYV = 152;
export declare const COLOR_BGRA2YUV_YV12 = 134;
export declare const COLOR_BGRA2YUV_YVYU = 154;
export declare const COLOR_BayerBG2BGR = 46;
export declare const COLOR_BayerBG2BGRA = 139;
export declare const COLOR_BayerBG2BGR_EA = 135;
export declare const COLOR_BayerBG2BGR_VNG = 62;
export declare const COLOR_BayerBG2GRAY = 86;
export declare const COLOR_BayerBG2RGB = 48;
export declare const COLOR_BayerBG2RGBA = 141;
export declare const COLOR_BayerBG2RGB_EA = 137;
export declare const COLOR_BayerBG2RGB_VNG = 64;
export declare const COLOR_BayerBGGR2BGR = 48;
export declare const COLOR_BayerBGGR2BGRA = 141;
export declare const COLOR_BayerBGGR2BGR_EA = 137;
export declare const COLOR_BayerBGGR2BGR_VNG = 64;
export declare const COLOR_BayerBGGR2GRAY = 88;
export declare const COLOR_BayerBGGR2RGB = 46;
export declare const COLOR_BayerBGGR2RGBA = 139;
export declare const COLOR_BayerBGGR2RGB_EA = 135;
export declare const COLOR_BayerBGGR2RGB_VNG = 62;
export declare const COLOR_BayerGB2BGR = 47;
export declare const COLOR_BayerGB2BGRA = 140;
export declare const COLOR_BayerGB2BGR_EA = 136;
export declare const COLOR_BayerGB2BGR_VNG = 63;
export declare const COLOR_BayerGB2GRAY = 87;
export declare const COLOR_BayerGB2RGB = 49;
export declare const COLOR_BayerGB2RGBA = 142;
export declare const COLOR_BayerGB2RGB_EA = 138;
export declare const COLOR_BayerGB2RGB_VNG = 65;
export declare const COLOR_BayerGBRG2BGR = 49;
export declare const COLOR_BayerGBRG2BGRA = 142;
export declare const COLOR_BayerGBRG2BGR_EA = 138;
export declare const COLOR_BayerGBRG2BGR_VNG = 65;
export declare const COLOR_BayerGBRG2GRAY = 89;
export declare const COLOR_BayerGBRG2RGB = 47;
export declare const COLOR_BayerGBRG2RGBA = 140;
export declare const COLOR_BayerGBRG2RGB_EA = 136;
export declare const COLOR_BayerGBRG2RGB_VNG = 63;
export declare const COLOR_BayerGR2BGR = 49;
export declare const COLOR_BayerGR2BGRA = 142;
export declare const COLOR_BayerGR2BGR_EA = 138;
export declare const COLOR_BayerGR2BGR_VNG = 65;
export declare const COLOR_BayerGR2GRAY = 89;
export declare const COLOR_BayerGR2RGB = 47;
export declare const COLOR_BayerGR2RGBA = 140;
export declare const COLOR_BayerGR2RGB_EA = 136;
export declare const COLOR_BayerGR2RGB_VNG = 63;
export declare const COLOR_BayerGRBG2BGR = 47;
export declare const COLOR_BayerGRBG2BGRA = 140;
export declare const COLOR_BayerGRBG2BGR_EA = 136;
export declare const COLOR_BayerGRBG2BGR_VNG = 63;
export declare const COLOR_BayerGRBG2GRAY = 87;
export declare const COLOR_BayerGRBG2RGB = 49;
export declare const COLOR_BayerGRBG2RGBA = 142;
export declare const COLOR_BayerGRBG2RGB_EA = 138;
export declare const COLOR_BayerGRBG2RGB_VNG = 65;
export declare const COLOR_BayerRG2BGR = 48;
export declare const COLOR_BayerRG2BGRA = 141;
export declare const COLOR_BayerRG2BGR_EA = 137;
export declare const COLOR_BayerRG2BGR_VNG = 64;
export declare const COLOR_BayerRG2GRAY = 88;
export declare const COLOR_BayerRG2RGB = 46;
export declare const COLOR_BayerRG2RGBA = 139;
export declare const COLOR_BayerRG2RGB_EA = 135;
export declare const COLOR_BayerRG2RGB_VNG = 62;
export declare const COLOR_BayerRGGB2BGR = 46;
export declare const COLOR_BayerRGGB2BGRA = 139;
export declare const COLOR_BayerRGGB2BGR_EA = 135;
export declare const COLOR_BayerRGGB2BGR_VNG = 62;
export declare const COLOR_BayerRGGB2GRAY = 86;
export declare const COLOR_BayerRGGB2RGB = 48;
export declare const COLOR_BayerRGGB2RGBA = 141;
export declare const COLOR_BayerRGGB2RGB_EA = 137;
export declare const COLOR_BayerRGGB2RGB_VNG = 64;
export declare const COLOR_COLORCVT_MAX = 155;
export declare const COLOR_GRAY2BGR = 8;
export declare const COLOR_GRAY2BGR555 = 30;
export declare const COLOR_GRAY2BGR565 = 20;
export declare const COLOR_GRAY2BGRA = 9;
export declare const COLOR_GRAY2RGB = 8;
export declare const COLOR_GRAY2RGBA = 9;
export declare const COLOR_HLS2BGR = 60;
export declare const COLOR_HLS2BGR_FULL = 72;
export declare const COLOR_HLS2RGB = 61;
export declare const COLOR_HLS2RGB_FULL = 73;
export declare const COLOR_HSV2BGR = 54;
export declare const COLOR_HSV2BGR_FULL = 70;
export declare const COLOR_HSV2RGB = 55;
export declare const COLOR_HSV2RGB_FULL = 71;
export declare const COLOR_LBGR2Lab = 74;
export declare const COLOR_LBGR2Luv = 76;
export declare const COLOR_LRGB2Lab = 75;
export declare const COLOR_LRGB2Luv = 77;
export declare const COLOR_Lab2BGR = 56;
export declare const COLOR_Lab2LBGR = 78;
export declare const COLOR_Lab2LRGB = 79;
export declare const COLOR_Lab2RGB = 57;
export declare const COLOR_Luv2BGR = 58;
export declare const COLOR_Luv2LBGR = 80;
export declare const COLOR_Luv2LRGB = 81;
export declare const COLOR_Luv2RGB = 59;
export declare const COLOR_RGB2BGR = 4;
export declare const COLOR_RGB2BGR555 = 23;
export declare const COLOR_RGB2BGR565 = 13;
export declare const COLOR_RGB2BGRA = 2;
export declare const COLOR_RGB2GRAY = 7;
export declare const COLOR_RGB2HLS = 53;
export declare const COLOR_RGB2HLS_FULL = 69;
export declare const COLOR_RGB2HSV = 41;
export declare const COLOR_RGB2HSV_FULL = 67;
export declare const COLOR_RGB2Lab = 45;
export declare const COLOR_RGB2Luv = 51;
export declare const COLOR_RGB2RGBA = 0;
export declare const COLOR_RGB2XYZ = 33;
export declare const COLOR_RGB2YCrCb = 37;
export declare const COLOR_RGB2YUV = 83;
export declare const COLOR_RGB2YUV_I420 = 127;
export declare const COLOR_RGB2YUV_IYUV = 127;
export declare const COLOR_RGB2YUV_UYNV = 143;
export declare const COLOR_RGB2YUV_UYVY = 143;
export declare const COLOR_RGB2YUV_Y422 = 143;
export declare const COLOR_RGB2YUV_YUNV = 147;
export declare const COLOR_RGB2YUV_YUY2 = 147;
export declare const COLOR_RGB2YUV_YUYV = 147;
export declare const COLOR_RGB2YUV_YV12 = 131;
export declare const COLOR_RGB2YUV_YVYU = 149;
export declare const COLOR_RGBA2BGR = 3;
export declare const COLOR_RGBA2BGR555 = 27;
export declare const COLOR_RGBA2BGR565 = 17;
export declare const COLOR_RGBA2BGRA = 5;
export declare const COLOR_RGBA2GRAY = 11;
export declare const COLOR_RGBA2RGB = 1;
export declare const COLOR_RGBA2YUV_I420 = 129;
export declare const COLOR_RGBA2YUV_IYUV = 129;
export declare const COLOR_RGBA2YUV_UYNV = 145;
export declare const COLOR_RGBA2YUV_UYVY = 145;
export declare const COLOR_RGBA2YUV_Y422 = 145;
export declare const COLOR_RGBA2YUV_YUNV = 151;
export declare const COLOR_RGBA2YUV_YUY2 = 151;
export declare const COLOR_RGBA2YUV_YUYV = 151;
export declare const COLOR_RGBA2YUV_YV12 = 133;
export declare const COLOR_RGBA2YUV_YVYU = 153;
export declare const COLOR_RGBA2mRGBA = 125;
export declare const COLOR_SPACE_ADOBE_RGB = 2;
export declare const COLOR_SPACE_ADOBE_RGBL = 3;
export declare const COLOR_SPACE_APPLE_RGB = 10;
export declare const COLOR_SPACE_APPLE_RGBL = 11;
export declare const COLOR_SPACE_DCI_P3_RGB = 8;
export declare const COLOR_SPACE_DCI_P3_RGBL = 9;
export declare const COLOR_SPACE_LAB_A_10 = 33;
export declare const COLOR_SPACE_LAB_A_2 = 32;
export declare const COLOR_SPACE_LAB_D50_10 = 31;
export declare const COLOR_SPACE_LAB_D50_2 = 29;
export declare const COLOR_SPACE_LAB_D55_10 = 35;
export declare const COLOR_SPACE_LAB_D55_2 = 34;
export declare const COLOR_SPACE_LAB_D65_10 = 30;
export declare const COLOR_SPACE_LAB_D65_2 = 28;
export declare const COLOR_SPACE_LAB_D75_10 = 37;
export declare const COLOR_SPACE_LAB_D75_2 = 36;
export declare const COLOR_SPACE_LAB_E_10 = 39;
export declare const COLOR_SPACE_LAB_E_2 = 38;
export declare const COLOR_SPACE_PRO_PHOTO_RGB = 6;
export declare const COLOR_SPACE_PRO_PHOTO_RGBL = 7;
export declare const COLOR_SPACE_REC_2020_RGB = 14;
export declare const COLOR_SPACE_REC_2020_RGBL = 15;
export declare const COLOR_SPACE_REC_709_RGB = 12;
export declare const COLOR_SPACE_REC_709_RGBL = 13;
export declare const COLOR_SPACE_SRGB = 0;
export declare const COLOR_SPACE_SRGBL = 1;
export declare const COLOR_SPACE_WIDE_GAMUT_RGB = 4;
export declare const COLOR_SPACE_WIDE_GAMUT_RGBL = 5;
export declare const COLOR_SPACE_XYZ_A_10 = 21;
export declare const COLOR_SPACE_XYZ_A_2 = 20;
export declare const COLOR_SPACE_XYZ_D50_10 = 19;
export declare const COLOR_SPACE_XYZ_D50_2 = 17;
export declare const COLOR_SPACE_XYZ_D55_10 = 23;
export declare const COLOR_SPACE_XYZ_D55_2 = 22;
export declare const COLOR_SPACE_XYZ_D65_10 = 18;
export declare const COLOR_SPACE_XYZ_D65_2 = 16;
export declare const COLOR_SPACE_XYZ_D75_10 = 25;
export declare const COLOR_SPACE_XYZ_D75_2 = 24;
export declare const COLOR_SPACE_XYZ_E_10 = 27;
export declare const COLOR_SPACE_XYZ_E_2 = 26;
export declare const COLOR_XYZ2BGR = 34;
export declare const COLOR_XYZ2RGB = 35;
export declare const COLOR_YCrCb2BGR = 38;
export declare const COLOR_YCrCb2RGB = 39;
export declare const COLOR_YUV2BGR = 84;
export declare const COLOR_YUV2BGRA_I420 = 105;
export declare const COLOR_YUV2BGRA_IYUV = 105;
export declare const COLOR_YUV2BGRA_NV12 = 95;
export declare const COLOR_YUV2BGRA_NV21 = 97;
export declare const COLOR_YUV2BGRA_UYNV = 112;
export declare const COLOR_YUV2BGRA_UYVY = 112;
export declare const COLOR_YUV2BGRA_Y422 = 112;
export declare const COLOR_YUV2BGRA_YUNV = 120;
export declare const COLOR_YUV2BGRA_YUY2 = 120;
export declare const COLOR_YUV2BGRA_YUYV = 120;
export declare const COLOR_YUV2BGRA_YV12 = 103;
export declare const COLOR_YUV2BGRA_YVYU = 122;
export declare const COLOR_YUV2BGR_I420 = 101;
export declare const COLOR_YUV2BGR_IYUV = 101;
export declare const COLOR_YUV2BGR_NV12 = 91;
export declare const COLOR_YUV2BGR_NV21 = 93;
export declare const COLOR_YUV2BGR_UYNV = 108;
export declare const COLOR_YUV2BGR_UYVY = 108;
export declare const COLOR_YUV2BGR_Y422 = 108;
export declare const COLOR_YUV2BGR_YUNV = 116;
export declare const COLOR_YUV2BGR_YUY2 = 116;
export declare const COLOR_YUV2BGR_YUYV = 116;
export declare const COLOR_YUV2BGR_YV12 = 99;
export declare const COLOR_YUV2BGR_YVYU = 118;
export declare const COLOR_YUV2GRAY_420 = 106;
export declare const COLOR_YUV2GRAY_I420 = 106;
export declare const COLOR_YUV2GRAY_IYUV = 106;
export declare const COLOR_YUV2GRAY_NV12 = 106;
export declare const COLOR_YUV2GRAY_NV21 = 106;
export declare const COLOR_YUV2GRAY_UYNV = 123;
export declare const COLOR_YUV2GRAY_UYVY = 123;
export declare const COLOR_YUV2GRAY_Y422 = 123;
export declare const COLOR_YUV2GRAY_YUNV = 124;
export declare const COLOR_YUV2GRAY_YUY2 = 124;
export declare const COLOR_YUV2GRAY_YUYV = 124;
export declare const COLOR_YUV2GRAY_YV12 = 106;
export declare const COLOR_YUV2GRAY_YVYU = 124;
export declare const COLOR_YUV2RGB = 85;
export declare const COLOR_YUV2RGBA_I420 = 104;
export declare const COLOR_YUV2RGBA_IYUV = 104;
export declare const COLOR_YUV2RGBA_NV12 = 94;
export declare const COLOR_YUV2RGBA_NV21 = 96;
export declare const COLOR_YUV2RGBA_UYNV = 111;
export declare const COLOR_YUV2RGBA_UYVY = 111;
export declare const COLOR_YUV2RGBA_Y422 = 111;
export declare const COLOR_YUV2RGBA_YUNV = 119;
export declare const COLOR_YUV2RGBA_YUY2 = 119;
export declare const COLOR_YUV2RGBA_YUYV = 119;
export declare const COLOR_YUV2RGBA_YV12 = 102;
export declare const COLOR_YUV2RGBA_YVYU = 121;
export declare const COLOR_YUV2RGB_I420 = 100;
export declare const COLOR_YUV2RGB_IYUV = 100;
export declare const COLOR_YUV2RGB_NV12 = 90;
export declare const COLOR_YUV2RGB_NV21 = 92;
export declare const COLOR_YUV2RGB_UYNV = 107;
export declare const COLOR_YUV2RGB_UYVY = 107;
export declare const COLOR_YUV2RGB_Y422 = 107;
export declare const COLOR_YUV2RGB_YUNV = 115;
export declare const COLOR_YUV2RGB_YUY2 = 115;
export declare const COLOR_YUV2RGB_YUYV = 115;
export declare const COLOR_YUV2RGB_YV12 = 98;
export declare const COLOR_YUV2RGB_YVYU = 117;
export declare const COLOR_YUV420p2BGR = 99;
export declare const COLOR_YUV420p2BGRA = 103;
export declare const COLOR_YUV420p2GRAY = 106;
export declare const COLOR_YUV420p2RGB = 98;
export declare const COLOR_YUV420p2RGBA = 102;
export declare const COLOR_YUV420sp2BGR = 93;
export declare const COLOR_YUV420sp2BGRA = 97;
export declare const COLOR_YUV420sp2GRAY = 106;
export declare const COLOR_YUV420sp2RGB = 92;
export declare const COLOR_YUV420sp2RGBA = 96;
export declare const COLOR_mRGBA2RGBA = 126;
export declare const THRESH_BINARY = 0;
export declare const THRESH_BINARY_INV = 1;
export declare const THRESH_DRYRUN = 128;
export declare const THRESH_MASK = 7;
export declare const THRESH_OTSU = 8;
export declare const THRESH_TOZERO = 3;
export declare const THRESH_TOZERO_INV = 4;
export declare const THRESH_TRIANGLE = 16;
export declare const THRESH_TRUNC = 2;
export declare const MORPH_BLACKHAT = 6;
export declare const MORPH_CLOSE = 3;
export declare const MORPH_CROSS = 1;
export declare const MORPH_DIAMOND = 3;
export declare const MORPH_DILATE = 1;
export declare const MORPH_ELLIPSE = 2;
export declare const MORPH_ERODE = 0;
export declare const MORPH_GRADIENT = 4;
export declare const MORPH_HITMISS = 7;
export declare const MORPH_OPEN = 2;
export declare const MORPH_RECT = 0;
export declare const MORPH_TOPHAT = 5;
export declare const RETR_CCOMP = 2;
export declare const RETR_EXTERNAL = 0;
export declare const RETR_FLOODFILL = 4;
export declare const RETR_LIST = 1;
export declare const RETR_TREE = 3;
export declare const CHAIN_APPROX_NONE = 1;
export declare const CHAIN_APPROX_SIMPLE = 2;
export declare const CHAIN_APPROX_TC89_KCOS = 4;
export declare const CHAIN_APPROX_TC89_L1 = 3;
export declare const INTER_AREA = 3;
export declare const INTER_CUBIC = 2;
export declare const INTER_LANCZOS4 = 4;
export declare const INTER_LINEAR = 1;
export declare const INTER_LINEAR_EXACT = 5;
export declare const INTER_NEAREST = 0;
export declare const INTER_NEAREST_EXACT = 6;
export declare const BORDER_CONSTANT = 0;
export declare const BORDER_DEFAULT = 4;
export declare const BORDER_ISOLATED = 16;
export declare const BORDER_REFLECT = 2;
export declare const BORDER_REFLECT101 = 4;
export declare const BORDER_REFLECT_101 = 4;
export declare const BORDER_REPLICATE = 1;
export declare const BORDER_TRANSPARENT = 5;
export declare const BORDER_WRAP = 3;
export declare const TM_CCOEFF = 4;
export declare const TM_CCOEFF_NORMED = 5;
export declare const TM_CCORR = 2;
export declare const TM_CCORR_NORMED = 3;
export declare const TM_SQDIFF = 0;
export declare const TM_SQDIFF_NORMED = 1;
export declare const NORM_HAMMING = 6;
export declare const NORM_HAMMING2 = 7;
export declare const NORM_INF = 1;
export declare const NORM_L1 = 2;
export declare const NORM_L2 = 4;
export declare const NORM_L2SQR = 5;
export declare const NORM_MINMAX = 32;
export declare const NORM_RELATIVE = 8;
export declare const CMP_EQ = 0;
export declare const CMP_GE = 2;
export declare const CMP_GT = 1;
export declare const CMP_LE = 4;
export declare const CMP_LT = 3;
export declare const CMP_NE = 5;
export declare const FONT_HERSHEY_COMPLEX = 3;
export declare const FONT_HERSHEY_COMPLEX_SMALL = 5;
export declare const FONT_HERSHEY_DUPLEX = 2;
export declare const FONT_HERSHEY_PLAIN = 1;
export declare const FONT_HERSHEY_SCRIPT_COMPLEX = 7;
export declare const FONT_HERSHEY_SCRIPT_SIMPLEX = 6;
export declare const FONT_HERSHEY_SIMPLEX = 0;
export declare const FONT_HERSHEY_TRIPLEX = 4;
export declare const LINE_4 = 4;
export declare const LINE_8 = 8;
export declare const LINE_AA = 16;
export declare const COLORMAP_AUTUMN = 0;
export declare const COLORMAP_BONE = 1;
export declare const COLORMAP_CIVIDIS = 17;
export declare const COLORMAP_COOL = 8;
export declare const COLORMAP_DEEPGREEN = 21;
export declare const COLORMAP_HOT = 11;
export declare const COLORMAP_HSV = 9;
export declare const COLORMAP_INFERNO = 14;
export declare const COLORMAP_JET = 2;
export declare const COLORMAP_MAGMA = 13;
export declare const COLORMAP_OCEAN = 5;
export declare const COLORMAP_PARULA = 12;
export declare const COLORMAP_PINK = 10;
export declare const COLORMAP_PLASMA = 15;
export declare const COLORMAP_RAINBOW = 4;
export declare const COLORMAP_SPRING = 7;
export declare const COLORMAP_SUMMER = 6;
export declare const COLORMAP_TURBO = 20;
export declare const COLORMAP_TWILIGHT = 18;
export declare const COLORMAP_TWILIGHT_SHIFTED = 19;
export declare const COLORMAP_VIRIDIS = 16;
export declare const COLORMAP_WINTER = 3;
export declare const DIST_C = 3;
export declare const DIST_FAIR = 5;
export declare const DIST_HUBER = 7;
export declare const DIST_L1 = 1;
export declare const DIST_L12 = 4;
export declare const DIST_L2 = 2;
export declare const DIST_LABEL_CCOMP = 0;
export declare const DIST_LABEL_PIXEL = 1;
export declare const DIST_MASK_3 = 3;
export declare const DIST_MASK_5 = 5;
export declare const DIST_MASK_PRECISE = 0;
export declare const DIST_USER = -1;
export declare const DIST_WELSCH = 6;
export declare const HOUGH_GRADIENT = 3;
export declare const HOUGH_GRADIENT_ALT = 4;
export declare const HOUGH_MULTI_SCALE = 2;
export declare const HOUGH_PROBABILISTIC = 1;
export declare const HOUGH_STANDARD = 0;
export declare const KMEANS_PP_CENTERS = 2;
export declare const KMEANS_RANDOM_CENTERS = 0;
export declare const KMEANS_USE_INITIAL_LABELS = 1;
export declare const HISTCMP_BHATTACHARYYA = 3;
export declare const HISTCMP_CHISQR = 1;
export declare const HISTCMP_CHISQR_ALT = 4;
export declare const HISTCMP_CORREL = 0;
export declare const HISTCMP_HELLINGER = 3;
export declare const HISTCMP_INTERSECT = 2;
export declare const HISTCMP_KL_DIV = 5;
export declare const CALIB_CB_ACCURACY = 32;
export declare const CALIB_CB_ADAPTIVE_THRESH = 1;
export declare const CALIB_CB_ASYMMETRIC_GRID = 2;
export declare const CALIB_CB_CLUSTERING = 4;
export declare const CALIB_CB_EXHAUSTIVE = 16;
export declare const CALIB_CB_FAST_CHECK = 8;
export declare const CALIB_CB_FILTER_QUADS = 4;
export declare const CALIB_CB_LARGER = 64;
export declare const CALIB_CB_MARKER = 128;
export declare const CALIB_CB_NORMALIZE_IMAGE = 2;
export declare const CALIB_CB_PLAIN = 256;
export declare const CALIB_CB_SYMMETRIC_GRID = 1;
export declare const CALIB_CHECK_COND = 16777216;
export declare const CALIB_DISABLE_SCHUR_COMPLEMENT = 262144;
export declare const CALIB_FIX_ASPECT_RATIO = 2;
export declare const CALIB_FIX_FOCAL_LENGTH = 16;
export declare const CALIB_FIX_INTRINSIC = 256;
export declare const CALIB_FIX_K1 = 32;
export declare const CALIB_FIX_K2 = 64;
export declare const CALIB_FIX_K3 = 128;
export declare const CALIB_FIX_K4 = 2048;
export declare const CALIB_FIX_K5 = 4096;
export declare const CALIB_FIX_K6 = 8192;
export declare const CALIB_FIX_PRINCIPAL_POINT = 4;
export declare const CALIB_FIX_S1_S2_S3_S4 = 65536;
export declare const CALIB_FIX_SKEW = 33554432;
export declare const CALIB_FIX_TANGENT_DIST = 2097152;
export declare const CALIB_FIX_TAUX_TAUY = 524288;
export declare const CALIB_HAND_EYE_ANDREFF = 3;
export declare const CALIB_HAND_EYE_DANIILIDIS = 4;
export declare const CALIB_HAND_EYE_HORAUD = 2;
export declare const CALIB_HAND_EYE_PARK = 1;
export declare const CALIB_HAND_EYE_TSAI = 0;
export declare const CALIB_MODEL_FISHEYE = 1;
export declare const CALIB_MODEL_PINHOLE = 0;
export declare const CALIB_RATIONAL_MODEL = 16384;
export declare const CALIB_RECOMPUTE_EXTRINSIC = 8388608;
export declare const CALIB_ROBOT_WORLD_HAND_EYE_LI = 1;
export declare const CALIB_ROBOT_WORLD_HAND_EYE_SHAH = 0;
export declare const CALIB_SAME_FOCAL_LENGTH = 512;
export declare const CALIB_STEREO_REGISTRATION = 67108864;
export declare const CALIB_THIN_PRISM_MODEL = 32768;
export declare const CALIB_TILTED_MODEL = 262144;
export declare const CALIB_USE_EXTRINSIC_GUESS = 4194304;
export declare const CALIB_USE_INTRINSIC_GUESS = 1;
export declare const CALIB_USE_LU = 131072;
export declare const CALIB_USE_QR = 1048576;
export declare const CALIB_ZERO_DISPARITY = 1024;
export declare const CALIB_ZERO_TANGENT_DIST = 8;
export declare const WARP_FILL_OUTLIERS = 8;
export declare const WARP_INVERSE_MAP = 16;
export declare const WARP_POLAR_LINEAR = 0;
export declare const WARP_POLAR_LOG = 256;
export declare const WARP_RELATIVE_MAP = 32;
export declare const INPAINT_NS = 0;
export declare const INPAINT_TELEA = 1;
export declare const RANSAC = 8;
export declare const CONTOURS_MATCH_I1 = 1;
export declare const CONTOURS_MATCH_I2 = 2;
export declare const CONTOURS_MATCH_I3 = 3;
export declare const LMEDS = 4;
export declare const RHO = 16;
export declare const IMREAD_UNCHANGED = -1;
export declare const IMREAD_GRAYSCALE = 0;
export declare const IMREAD_COLOR = 1;
export declare const IMREAD_ANYDEPTH = 2;
export declare const IMREAD_ANYCOLOR = 4;
export declare const IMREAD_IGNORE_ORIENTATION = 128;
export declare const ADAPTIVE_THRESH_MEAN_C = 0;
export declare const ADAPTIVE_THRESH_GAUSSIAN_C = 1;
export declare const DECOMP_LU = 0;
export declare const DECOMP_SVD = 1;
export declare const DECOMP_EIG = 2;
export declare const DECOMP_CHOLESKY = 3;
export declare const DECOMP_QR = 4;
export declare const DECOMP_NORMAL = 16;
export declare const TERM_CRITERIA_COUNT = 1;
export declare const TERM_CRITERIA_MAX_ITER = 1;
export declare const TERM_CRITERIA_EPS = 2;

/** `cvtColor(src, code)` 返回新图；`cvtColor(src, dst, code)` 写进 dst（OpenCV.js 形状）。 */
export declare function cvtColor(src: Mat, code: ColorCode | number): Mat;
export declare function cvtColor(src: Mat, dst: Mat, code: ColorCode | number): void;
/** OpenCV.js 的 inRange(src, lowerb, upperb[, dst]) —— 注意 dst 在**第四个**位置。
 *  给了 dst 就写进去并**无返回**（与 OpenCV.js 一致）, 否则返回新图。 */
export declare function inRange(src: Mat, lower: ColorArg, upper: ColorArg, dst: Mat): void;
export declare function inRange(src: Mat, lower: ColorArg, upper: ColorArg): Mat;
/** OpenCV.js 的 cv.equalizeHist(src[, dst]) */
export declare function equalizeHist(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function equalizeHist(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.LUT(src, lut[, dst]) */
export declare function LUT(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function LUT(src: Mat, table: number[], dst?: Mat): Mat;
/** OpenCV.js 的 cv.applyColorMap(src, colormap[, dst])；也收 (src, dst, colormap) 的写法。 */
export declare function applyColorMap(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function applyColorMap(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 形状：`threshold(src, dst, thresh, maxval, type)` 回**实际阈值**(number)；
 *  不给 dst 时按本包的形状回 `ThresholdResult`。 */
export declare function threshold(src: Mat, dst: Mat, thresh: number, maxval: number, type?: ThreshType | number): number;
export declare function threshold(src: Mat, thresh: number, maxval: number, type?: ThreshType | number): ThresholdResult;
/** OpenCV.js 的 adaptiveThreshold(src, dst, maxValue, adaptiveMethod, thresholdType, blockSize, C)。
 *  adaptiveMethod 收 0/1（cv.ADAPTIVE_THRESH_MEAN_C / GAUSSIAN_C）或 'mean'/'gaussian'。 */
export declare function adaptiveThreshold(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function adaptiveThreshold(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.blur(src, dst, ksize, borderType) */
export declare function blur(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function blur(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.boxFilter(src, dst, ddepth, ksize, ...)（ddepth/anchor/borderType 目前忽略） */
export declare function boxFilter(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function boxFilter(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.GaussianBlur(src, dst, ksize, sigmaX, sigmaY, borderType) */
export declare function GaussianBlur(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function GaussianBlur(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.medianBlur(src, dst, ksize) */
export declare function medianBlur(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function medianBlur(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.bilateralFilter(src, dst, d, sigmaColor, sigmaSpace) */
export declare function bilateralFilter(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function bilateralFilter(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.filter2D(src, dst, ddepth, kernel, anchor, delta, borderType) */
export declare function filter2D(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function filter2D(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.Sobel(src, dst, ddepth, dx, dy, ksize, scale, delta, borderType) */
export declare function Sobel(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function Sobel(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.Scharr(src, dst, ddepth, dx, dy, scale, delta, borderType) */
export declare function Scharr(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function Scharr(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.Laplacian(src, dst, ddepth, ksize, scale, delta, borderType) */
export declare function Laplacian(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function Laplacian(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.Canny(image, edges, threshold1, threshold2, apertureSize, L2gradient) */
export declare function Canny(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function Canny(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.getStructuringElement(shape, ksize)；shape 也收 cv.MORPH_RECT/ELLIPSE/CROSS 那几个数。 */
export declare function getStructuringElement(shape?: any, ksize?: Size2): Mat;
/** OpenCV.js 的 cv.erode(src, dst, kernel, anchor, iterations, borderType) */
export declare function erode(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function erode(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.dilate(src, dst, kernel, anchor, iterations, borderType) */
export declare function dilate(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function dilate(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.morphologyEx(src, dst, op, kernel, anchor, iterations, borderType) */
export declare function morphologyEx(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function morphologyEx(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 `connectedComponents(image, labels, connectivity, ltype)` —— 给了 labels 就写进去
 *  并回**连通域个数**（与 OpenCV.js 一致）；不给则按本包的形状回 `{labels,count,stats,centroids}`。
 *  ⚠ `ltype` 只支持 CV_32S(4)（后端固定用 32S 标签）, 给别的值会报错而不是悄悄忽略。 */
export declare function connectedComponents(image: Mat, labels: Mat, connectivity?: 4 | 8, ltype?: number): number;
export declare function connectedComponents(image: Mat, connectivity?: 4 | 8): ComponentsResult;
/** OpenCV.js 的 `connectedComponentsWithStats(image, labels, stats, centroids, connectivity, ltype)`：
 *  labels（Nx1 CV_32S）/ stats（N×5 CV_32S）/ centroids（N×2 CV_64F）都是**出参**，回连通域个数。
 *  不给这三个出参时（本包扩展）回 `{labels, count, stats, centroids}`。 */
export declare function connectedComponentsWithStats(image: Mat, labels: Mat, stats: Mat, centroids: Mat, connectivity?: 4 | 8, ltype?: number): number;
export declare function connectedComponentsWithStats(image: Mat, connectivity?: 4 | 8): ComponentsResult;
/** OpenCV.js 的 cv.resize(src, dst, dsize, fx, fy, interpolation)；dsize 给 cv.Size 或 [w,h] */
export declare function resize(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function resize(src: Mat, ...rest: any[]): Mat;
export declare function resize(src: Mat, dst: Mat, dsize?: any, fx?: number, fy?: number, interpolation?: any): Mat;
export declare function resize(src: Mat, sizeOrOpts: any, interpolation?: any): Mat;
/** OpenCV.js 的 cv.flip(src, dst, flipCode)：0=上下翻, 正数=左右翻, 负数=既上下又左右 */
export declare function flip(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function flip(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.warpAffine(src, dst, M, dsize) */
export declare function warpAffine(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function warpAffine(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.warpPerspective(src, dst, M, dsize) */
export declare function warpPerspective(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function warpPerspective(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.copyMakeBorder(src, dst, top, bottom, left, right, borderType, value) */
export declare function copyMakeBorder(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function copyMakeBorder(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.split(src, mv)：各通道推进 MatVector, **无返回**。
 *  不给 mv 时（本包扩展）返回 `Mat[]`。 */
export declare function split(src: Mat, mv: MatVector): void;
export declare function split(src: Mat): Mat[];
/** OpenCV.js 的 cv.merge(mv, dst)：写进 dst, **无返回**（也给本包的 `merge(mats)` 回一张新图）。 */
export declare function merge(mv: any, dst: Mat): void;
export declare function merge(mv: any): Mat;
/** OpenCV.js 的 cv.addWeighted(src1, alpha, src2, beta, gamma, dst)：给了 dst 就无返回 */
export declare function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma: number, dst: Mat): void;
export declare function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma?: number): Mat;
export declare function add(a: Mat, b: Mat, dst: Mat): void;
export declare function add(a: Mat, b: Mat): Mat;
export declare function subtract(a: Mat, b: Mat, dst: Mat): void;
export declare function subtract(a: Mat, b: Mat): Mat;
/** OpenCV.js 的 cv.multiply(src1, src2, dst[, scale])（第 3 个参数是 Mat 就是 dst） */
export declare function multiply(a: Mat, b: Mat, dst: Mat, scale?: number): void;
export declare function multiply(a: Mat, b: Mat, scale?: number): Mat;
export declare function divide(a: Mat, b: Mat, dst: Mat, scale?: number): void;
export declare function divide(a: Mat, b: Mat, scale?: number): Mat;
export declare function absdiff(a: Mat, b: Mat, dst: Mat): void;
export declare function absdiff(a: Mat, b: Mat): Mat;
export declare function bitwise_and(a: Mat, b: Mat, dst: Mat): void;
export declare function bitwise_and(a: Mat, b: Mat): Mat;
export declare function bitwise_or(a: Mat, b: Mat, dst: Mat): void;
export declare function bitwise_or(a: Mat, b: Mat): Mat;
export declare function bitwise_xor(a: Mat, b: Mat, dst: Mat): void;
export declare function bitwise_xor(a: Mat, b: Mat): Mat;
/** OpenCV.js 的 cv.bitwise_not(src, dst) —— 取反（掩膜取反最常用） */
export declare function bitwise_not(src: Mat, dst: Mat): void;
export declare function bitwise_not(src: Mat): Mat;
/** OpenCV.js 的 cv.min(src1, src2, dst) / cv.max(...) */
export declare function min(a: Mat, b: Mat, dst: Mat): void;
export declare function min(a: Mat, b: Mat): Mat;
export declare function max(a: Mat, b: Mat, dst: Mat): void;
export declare function max(a: Mat, b: Mat): Mat;
/** OpenCV.js 的 cv.normalize(src, dst, alpha, beta, norm_type) */
export declare function normalize(src: Mat, dst: Mat, ...rest: any[]): void;
export declare function normalize(src: Mat, ...rest: any[]): Mat;
/** OpenCV.js 的 cv.compare(src1, src2, dst, cmpop)：给了 dst 就无返回 */
export declare function compare(a: Mat, b: any, dst: Mat, cmpop?: CmpOp | number): void;
export declare function compare(a: Mat, b: any, cmpop?: CmpOp | number): Mat;
/** OpenCV.js 的 `findContours(image, contours, hierarchy, mode, method)`：结果填进出参
 *  （`contours` 里每项是**轮廓 Mat**, `hierarchy` 是 Mat）—— **返回 undefined**, 与 OpenCV.js 一致。
 *  派生量与它一样按需再问: contourArea / boundingRect / convexHull / approxPolyDP ...
 *  另: 只给 opts 的写法（`findContours(image, { mode, method })`）是本包的扩展, 直接回 `Contour[]`
 *  （每条轮廓的派生量都已算在里面, 代价是一次较大的 JSON, 见文件头）。 */
export declare function findContours(src: Mat, contours: MatVector, hierarchy?: Mat, mode?: RetrMode, method?: ChainMethod): void;
export declare function findContours(src: Mat, opts?: {
    mode?: RetrMode;
    method?: ChainMethod;
    approxEpsilon?: number;
}): Contour[];
/** OpenCV.js 的 `convexHull(cnt, hull[, clockwise, returnPoints])`：写进 `hull`, **无返回**。
 *  不给 hull 时（本包的扩展）返回那张 Mat —— 内容是 Nx1 的 32SC2 轮廓点（returnPoints=false 时是 32SC1 下标）。 */
export declare function convexHull(c: ContourLike, hull?: Mat, clockwise?: boolean, returnPoints?: boolean): any;
/** OpenCV.js 的 `approxPolyDP(cnt, approx, eps, closed)`：写进 `approx`, **无返回**。
 *  不给 out 时（本包的扩展）返回那张 Mat。⚠ `eps` 是**绝对**阈值（与 OpenCV.js 一致,
 *  不再是"占周长的比例"）。 */
export declare function approxPolyDP(c: ContourLike, approxCurve?: Mat | number | null, eps?: number, closed?: boolean): any;
/** OpenCV.js 的 `minMaxLoc(src[, mask])` -> `{minVal,maxVal,minLoc,maxLoc}`（定位点是带 .x/.y 的
 *  cv.Point）。mask 也接了（只统计掩膜里的像素, 与 OpenCV.js 一致）。 */
export declare function minMaxLoc(src: Mat, mask?: Mat): any;
/** OpenCV.js 的 `meanStdDev(src, mean, stddev[, mask])`：出参式（mean/stddev 是 Mat, 写进去;
 *  与 OpenCV.js 一样是 1×N 的 CV_64F）—— **无返回**。不给出参时（本包扩展）回 `{mean, stddev}`。 */
export declare function meanStdDev(src: Mat, mean: Mat, stddev: Mat, mask?: Mat): void;
export declare function meanStdDev(src: Mat, mask?: Mat): MeanStdDevResult;
/** OpenCV.js 的 `cv.HuMoments(moments, dst)`：把 7 个 Hu 不变矩写成 dst（1×7 的 CV_64F）, **无返回**。
 *  ⚠ opencv-js 的 `cv.moments()` 结果里**没有** hu（要自己调 HuMoments 算）; 我们的 moments() 直接带了
 *  `hu`, 所以这条只是把已有数字落成一张 Mat, 不回后端。 */
export declare function HuMoments(m: any, dst: Mat): void;
/** OpenCV.js 的 `matchTemplate(image, templ, result, method)` —— 写进 result, **无返回**。
 *  （也收我们原来的顺序 `(src, templ, method, result)`：第 3 个参数是不是 Mat 说了算。）
 *  不给 result 时（本包扩展）返回分数图。 */
export declare function matchTemplate(src: Mat, templ: Mat, result: Mat, method?: TmMethod | number): void;
export declare function matchTemplate(src: Mat, templ: Mat, method?: TmMethod | number): Mat;
/** OpenCV.js 的 cv.line(img, pt1, pt2, color, thickness, lineType, shift) */
export declare function line(img: Mat, pt1: Point2, pt2: Point2, color?: ColorArg, thickness?: any, lineType?: any): void;
/** OpenCV.js 的 cv.rectangle(img, pt1, pt2, color, thickness, lineType, shift)（也收 Rect） */
export declare function rectangle(img: Mat, a: any, b?: any, c?: any, d?: any, e?: any): void;
/** OpenCV.js 的 cv.circle(img, center, radius, color, thickness, lineType, shift) */
export declare function circle(img: Mat, center: any, radius?: number, color?: ColorArg, thickness?: any, lineType?: any): void;
/** OpenCV.js 的 cv.ellipse(img, center, axes, angle, startAngle, endAngle, color, thickness, ...) */
export declare function ellipse(img: Mat, center: any, axes?: any, angle?: any, startAngle?: any, endAngle?: any, color?: ColorArg, thickness?: any): void;
/** OpenCV.js 的 cv.polylines(img, pts, isClosed, color, thickness) —— pts 收 MatVector 或点数组 */
export declare function polylines(img: Mat, pts: any, isClosed?: any, color?: ColorArg, thickness?: any): void;
/** OpenCV.js 的 cv.putText(img, text, org, fontFace, fontScale, color, thickness, lineType) */
export declare function putText(img: Mat, text: string, org: Point2, fontFace?: any, fontScale?: any, color?: ColorArg, thickness?: any, lineType?: any): void;
/** OpenCV.js 的 `imread(filename, flags)`：flags 认 IMREAD_GRAYSCALE / IMREAD_COLOR（其余按原样读）。 */
export declare function imread(path: string, flags?: number): Mat;
/** OpenCV.js 的 `imencode(ext, img)` 回一张**装着编码字节的图**(8UC1)；本包的老形状
 *  `imencode(src, ext)` 回 dataURL 字符串 —— 按第一个参数是字符串还是 Mat 区分。 */
export declare function imencode(a: any, b?: any): any;
export declare function sqrt(src: Mat, dst: Mat): void;
export declare function sqrt(src: Mat): Mat;
export declare function log(src: Mat, dst: Mat): void;
export declare function log(src: Mat): Mat;
export declare function exp(src: Mat, dst: Mat): void;
export declare function exp(src: Mat): Mat;
export declare function pow(src: Mat, power: number, dst: Mat): void;
export declare function pow(src: Mat, power?: number): Mat;
export declare function magnitude(x: Mat, y: Mat, dst: Mat): void;
export declare function magnitude(x: Mat, y: Mat): Mat;
export declare function phase(x: Mat, y: Mat, angle: Mat, angleInDegrees?: boolean): void;
export declare function phase(x: Mat, y: Mat, angleInDegrees?: boolean): Mat;
export declare function cartToPolar(x: Mat, y: Mat, magnitude: Mat, angle: Mat, angleInDegrees?: boolean): void;
export declare function cartToPolar(x: Mat, y: Mat, angleInDegrees?: boolean): CartToPolarResult;
export declare function polarToCart(magnitude: Mat, angle: Mat, x: Mat, y: Mat, angleInDegrees?: boolean): void;
export declare function polarToCart(magnitude: Mat, angle: Mat, angleInDegrees?: boolean): PolarToCartResult;
export declare function pyrDown(src: Mat, dst: Mat, dstsize?: Size2, borderType?: number): void;
export declare function pyrDown(src: Mat, dstsize?: Size2): Mat;
export declare function pyrUp(src: Mat, dst: Mat, dstsize?: Size2, borderType?: number): void;
export declare function pyrUp(src: Mat, dstsize?: Size2): Mat;
export declare function reduce(src: Mat, dst: Mat, dim: number, rtype: number | string, dtype?: number): void;
export declare function reduce(src: Mat, dim?: number, rtype?: number | string, dtype?: number): Mat;
export declare function findNonZero(src: Mat, idx: Mat): void;
export declare function findNonZero(src: Mat): Mat;
export declare function integral(src: Mat, sum: Mat, sdepth?: number): void;
export declare function integral(src: Mat, sum: Mat, sqsum: Mat, sdepth?: number, sqdepth?: number): void;
export declare function integral(src: Mat, sum: Mat, sqsum: Mat, tilted: Mat, sdepth?: number, sqdepth?: number): void;
export declare function integral(src: Mat, opts?: {
    sdepth?: number;
    sqsum?: boolean;
    tilted?: boolean;
}): IntegralResult;
export declare function cornerHarris(src: Mat, dst: Mat, blockSize: number, ksize: number, k: number, borderType?: number): void;
export declare function cornerHarris(src: Mat, blockSize?: number, ksize?: number, k?: number, borderType?: number): Mat;
export declare function goodFeaturesToTrack(image: Mat, corners: Mat, maxCorners: number, qualityLevel: number, minDistance: number, mask?: Mat, blockSize?: number, useHarrisDetector?: boolean, k?: number): void;
export declare function goodFeaturesToTrack(image: Mat, maxCorners?: number, qualityLevel?: number, minDistance?: number, blockSize?: number, useHarrisDetector?: boolean, k?: number): Mat;
export declare function HoughCircles(image: Mat, circles: Mat, method: number | string, dp: number, minDist: number, param1?: number, param2?: number, minRadius?: number, maxRadius?: number): void;
export declare function HoughCircles(image: Mat, method?: number | string, dp?: number, minDist?: number, param1?: number, param2?: number, minRadius?: number, maxRadius?: number): Mat;
export declare function HoughLinesP(image: Mat, lines: Mat, rho: number, theta: number, threshold: number, minLineLength?: number, maxLineGap?: number): void;
export declare function HoughLinesP(image: Mat, rho?: number, theta?: number, threshold?: number, minLineLength?: number, maxLineGap?: number): Mat;
export declare function distanceTransform(src: Mat, dst: Mat, distanceType: number | string, maskSize: number | string, dstType?: number): void;
export declare function distanceTransform(src: Mat, dst: Mat, labels: Mat, distanceType: number | string, maskSize: number | string, labelType?: number): void;
export declare function distanceTransform(src: Mat, distanceType?: any, maskSize?: any, labels?: boolean, labelType?: any): DistanceTransformResult;
export declare function floodFill(image: Mat, seedPoint: Point2, newVal: ColorArg, loDiff?: ColorArg, upDiff?: ColorArg, flags?: number): number;
export declare function floodFill(image: Mat, mask: Mat, seedPoint: Point2, newVal: ColorArg, loDiff?: ColorArg, upDiff?: ColorArg, flags?: number): number;
/** OpenCV.js 的 `cv.watershed(image, markers)`：**就地**改 markers, 无返回 */
export declare function watershed(image: Mat, markers: Mat): void;
/** OpenCV.js 的 `cv.kmeans(data, K, bestLabels, criteria, attempts, flags[, centers])` -> 紧凑度 */
export declare function kmeans(data: Mat, K: number, bestLabels: Mat, criteria: any, attempts: number, flags: number, centers?: Mat): number;
export declare function kmeans(data: Mat, K?: number, criteria?: any, attempts?: number, flags?: number): KmeansResult;
/** OpenCV.js 的 `cv.drawContours(image, contours, contourIdx, color, ...)`：**就地**画, 无返回 */
export declare function drawContours(image: Mat, contours: any[], contourIdx: number, color: ColorArg, thickness?: number, lineType?: number, hierarchy?: Mat, maxLevel?: number, offset?: Point2): void;
/** OpenCV.js 的 `cv.minEnclosingCircle(points)` -> Circle */
export declare function minEnclosingCircle(points: any): Circle;
/** OpenCV.js 的 `cv.fitEllipse(points)` -> RotatedRect */
export declare function fitEllipse(points: any): RotatedRect;
export declare function fitLine(points: any, line: Mat, distType: number | string, param: number, reps: number, aeps: number): void;
export declare function fitLine(points: any, distType?: number | string, param?: number, reps?: number, aeps?: number): Mat;
/** OpenCV.js 的 `cv.convexityDefects(contour, convexhull, defects)`：写进出参, 无返回 */
export declare function convexityDefects(contour: any, convexhull: Mat | null, defects: Mat): void;
export declare function convexityDefects(contour: any): Mat;
/** OpenCV.js 的 `cv.calcBackProject([src], [channel], hist, dst, [lo, hi], scale)` */
export declare function calcBackProject(images: any, channels: any, hist: Mat, dst: Mat, ranges: any, scale?: number): void;
export declare function calcBackProject(src: Mat, hist: Mat, channel?: number, lo?: number, hi?: number, scale?: number): Mat;
/** OpenCV.js 的 `cv.findHomography(srcPoints, dstPoints[, method, ransacReprojThreshold, mask])` -> H */
export declare function findHomography(srcPoints: any, dstPoints: any, mask: Mat, method?: number | string, ransacReprojThreshold?: number): Mat;
export declare function findHomography(srcPoints: any, dstPoints: any, method?: number | string, ransacReprojThreshold?: number, mask?: Mat, maxIters?: number, confidence?: number): Mat;
export declare function undistort(src: Mat, dst: Mat, cameraMatrix: Mat, distCoeffs: Mat, newCameraMatrix?: Mat): void;
export declare function undistort(src: Mat, cameraMatrix: Mat, distCoeffs?: Mat, newCameraMatrix?: Mat): Mat;
export declare function remap(src: Mat, dst: Mat, map1: Mat, map2: Mat, interpolation: number | string, borderMode?: number | string, borderValue?: ColorArg): void;
export declare function remap(src: Mat, map1: Mat, map2?: Mat, interpolation?: number | string, borderMode?: number | string, borderValue?: ColorArg): Mat;
export declare function calcOpticalFlowFarneback(prev: Mat, next: Mat, flow: Mat, pyrScale: number, levels: number, winsize: number, iterations: number, polyN: number, polySigma: number, flags: number): void;
export declare function calcOpticalFlowFarneback(prev: Mat, next: Mat, pyrScale?: number, levels?: number, winsize?: number, iterations?: number, polyN?: number, polySigma?: number, flags?: number): Mat;
declare const cv: {
    CV_8U: 0;
    CV_8S: 1;
    CV_16U: 2;
    CV_16S: 3;
    CV_32S: 4;
    CV_32F: 5;
    CV_64F: 6;
    CV_8UC1: 0;
    CV_8UC2: 8;
    CV_8UC3: 16;
    CV_8UC4: 24;
    CV_8SC1: 1;
    CV_8SC2: 9;
    CV_8SC3: 17;
    CV_8SC4: 25;
    CV_16UC1: 2;
    CV_16UC2: 10;
    CV_16UC3: 18;
    CV_16UC4: 26;
    CV_16SC1: 3;
    CV_16SC2: 11;
    CV_16SC3: 19;
    CV_16SC4: 27;
    CV_32SC1: 4;
    CV_32SC2: 12;
    CV_32SC3: 20;
    CV_32SC4: 28;
    CV_32FC1: 5;
    CV_32FC2: 13;
    CV_32FC3: 21;
    CV_32FC4: 29;
    CV_64FC1: 6;
    CV_64FC2: 14;
    CV_64FC3: 22;
    CV_64FC4: 30;
    CV_16F: 7;
    COLOR_BGR2BGR555: 22;
    COLOR_BGR2BGR565: 12;
    COLOR_BGR2BGRA: 0;
    COLOR_BGR2GRAY: 6;
    COLOR_BGR2HLS: 52;
    COLOR_BGR2HLS_FULL: 68;
    COLOR_BGR2HSV: 40;
    COLOR_BGR2HSV_FULL: 66;
    COLOR_BGR2Lab: 44;
    COLOR_BGR2Luv: 50;
    COLOR_BGR2RGB: 4;
    COLOR_BGR2RGBA: 2;
    COLOR_BGR2XYZ: 32;
    COLOR_BGR2YCrCb: 36;
    COLOR_BGR2YUV: 82;
    COLOR_BGR2YUV_I420: 128;
    COLOR_BGR2YUV_IYUV: 128;
    COLOR_BGR2YUV_UYNV: 144;
    COLOR_BGR2YUV_UYVY: 144;
    COLOR_BGR2YUV_Y422: 144;
    COLOR_BGR2YUV_YUNV: 148;
    COLOR_BGR2YUV_YUY2: 148;
    COLOR_BGR2YUV_YUYV: 148;
    COLOR_BGR2YUV_YV12: 132;
    COLOR_BGR2YUV_YVYU: 150;
    COLOR_BGR5552BGR: 24;
    COLOR_BGR5552BGRA: 28;
    COLOR_BGR5552GRAY: 31;
    COLOR_BGR5552RGB: 25;
    COLOR_BGR5552RGBA: 29;
    COLOR_BGR5652BGR: 14;
    COLOR_BGR5652BGRA: 18;
    COLOR_BGR5652GRAY: 21;
    COLOR_BGR5652RGB: 15;
    COLOR_BGR5652RGBA: 19;
    COLOR_BGRA2BGR: 1;
    COLOR_BGRA2BGR555: 26;
    COLOR_BGRA2BGR565: 16;
    COLOR_BGRA2GRAY: 10;
    COLOR_BGRA2RGB: 3;
    COLOR_BGRA2RGBA: 5;
    COLOR_BGRA2YUV_I420: 130;
    COLOR_BGRA2YUV_IYUV: 130;
    COLOR_BGRA2YUV_UYNV: 146;
    COLOR_BGRA2YUV_UYVY: 146;
    COLOR_BGRA2YUV_Y422: 146;
    COLOR_BGRA2YUV_YUNV: 152;
    COLOR_BGRA2YUV_YUY2: 152;
    COLOR_BGRA2YUV_YUYV: 152;
    COLOR_BGRA2YUV_YV12: 134;
    COLOR_BGRA2YUV_YVYU: 154;
    COLOR_BayerBG2BGR: 46;
    COLOR_BayerBG2BGRA: 139;
    COLOR_BayerBG2BGR_EA: 135;
    COLOR_BayerBG2BGR_VNG: 62;
    COLOR_BayerBG2GRAY: 86;
    COLOR_BayerBG2RGB: 48;
    COLOR_BayerBG2RGBA: 141;
    COLOR_BayerBG2RGB_EA: 137;
    COLOR_BayerBG2RGB_VNG: 64;
    COLOR_BayerBGGR2BGR: 48;
    COLOR_BayerBGGR2BGRA: 141;
    COLOR_BayerBGGR2BGR_EA: 137;
    COLOR_BayerBGGR2BGR_VNG: 64;
    COLOR_BayerBGGR2GRAY: 88;
    COLOR_BayerBGGR2RGB: 46;
    COLOR_BayerBGGR2RGBA: 139;
    COLOR_BayerBGGR2RGB_EA: 135;
    COLOR_BayerBGGR2RGB_VNG: 62;
    COLOR_BayerGB2BGR: 47;
    COLOR_BayerGB2BGRA: 140;
    COLOR_BayerGB2BGR_EA: 136;
    COLOR_BayerGB2BGR_VNG: 63;
    COLOR_BayerGB2GRAY: 87;
    COLOR_BayerGB2RGB: 49;
    COLOR_BayerGB2RGBA: 142;
    COLOR_BayerGB2RGB_EA: 138;
    COLOR_BayerGB2RGB_VNG: 65;
    COLOR_BayerGBRG2BGR: 49;
    COLOR_BayerGBRG2BGRA: 142;
    COLOR_BayerGBRG2BGR_EA: 138;
    COLOR_BayerGBRG2BGR_VNG: 65;
    COLOR_BayerGBRG2GRAY: 89;
    COLOR_BayerGBRG2RGB: 47;
    COLOR_BayerGBRG2RGBA: 140;
    COLOR_BayerGBRG2RGB_EA: 136;
    COLOR_BayerGBRG2RGB_VNG: 63;
    COLOR_BayerGR2BGR: 49;
    COLOR_BayerGR2BGRA: 142;
    COLOR_BayerGR2BGR_EA: 138;
    COLOR_BayerGR2BGR_VNG: 65;
    COLOR_BayerGR2GRAY: 89;
    COLOR_BayerGR2RGB: 47;
    COLOR_BayerGR2RGBA: 140;
    COLOR_BayerGR2RGB_EA: 136;
    COLOR_BayerGR2RGB_VNG: 63;
    COLOR_BayerGRBG2BGR: 47;
    COLOR_BayerGRBG2BGRA: 140;
    COLOR_BayerGRBG2BGR_EA: 136;
    COLOR_BayerGRBG2BGR_VNG: 63;
    COLOR_BayerGRBG2GRAY: 87;
    COLOR_BayerGRBG2RGB: 49;
    COLOR_BayerGRBG2RGBA: 142;
    COLOR_BayerGRBG2RGB_EA: 138;
    COLOR_BayerGRBG2RGB_VNG: 65;
    COLOR_BayerRG2BGR: 48;
    COLOR_BayerRG2BGRA: 141;
    COLOR_BayerRG2BGR_EA: 137;
    COLOR_BayerRG2BGR_VNG: 64;
    COLOR_BayerRG2GRAY: 88;
    COLOR_BayerRG2RGB: 46;
    COLOR_BayerRG2RGBA: 139;
    COLOR_BayerRG2RGB_EA: 135;
    COLOR_BayerRG2RGB_VNG: 62;
    COLOR_BayerRGGB2BGR: 46;
    COLOR_BayerRGGB2BGRA: 139;
    COLOR_BayerRGGB2BGR_EA: 135;
    COLOR_BayerRGGB2BGR_VNG: 62;
    COLOR_BayerRGGB2GRAY: 86;
    COLOR_BayerRGGB2RGB: 48;
    COLOR_BayerRGGB2RGBA: 141;
    COLOR_BayerRGGB2RGB_EA: 137;
    COLOR_BayerRGGB2RGB_VNG: 64;
    COLOR_COLORCVT_MAX: 155;
    COLOR_GRAY2BGR: 8;
    COLOR_GRAY2BGR555: 30;
    COLOR_GRAY2BGR565: 20;
    COLOR_GRAY2BGRA: 9;
    COLOR_GRAY2RGB: 8;
    COLOR_GRAY2RGBA: 9;
    COLOR_HLS2BGR: 60;
    COLOR_HLS2BGR_FULL: 72;
    COLOR_HLS2RGB: 61;
    COLOR_HLS2RGB_FULL: 73;
    COLOR_HSV2BGR: 54;
    COLOR_HSV2BGR_FULL: 70;
    COLOR_HSV2RGB: 55;
    COLOR_HSV2RGB_FULL: 71;
    COLOR_LBGR2Lab: 74;
    COLOR_LBGR2Luv: 76;
    COLOR_LRGB2Lab: 75;
    COLOR_LRGB2Luv: 77;
    COLOR_Lab2BGR: 56;
    COLOR_Lab2LBGR: 78;
    COLOR_Lab2LRGB: 79;
    COLOR_Lab2RGB: 57;
    COLOR_Luv2BGR: 58;
    COLOR_Luv2LBGR: 80;
    COLOR_Luv2LRGB: 81;
    COLOR_Luv2RGB: 59;
    COLOR_RGB2BGR: 4;
    COLOR_RGB2BGR555: 23;
    COLOR_RGB2BGR565: 13;
    COLOR_RGB2BGRA: 2;
    COLOR_RGB2GRAY: 7;
    COLOR_RGB2HLS: 53;
    COLOR_RGB2HLS_FULL: 69;
    COLOR_RGB2HSV: 41;
    COLOR_RGB2HSV_FULL: 67;
    COLOR_RGB2Lab: 45;
    COLOR_RGB2Luv: 51;
    COLOR_RGB2RGBA: 0;
    COLOR_RGB2XYZ: 33;
    COLOR_RGB2YCrCb: 37;
    COLOR_RGB2YUV: 83;
    COLOR_RGB2YUV_I420: 127;
    COLOR_RGB2YUV_IYUV: 127;
    COLOR_RGB2YUV_UYNV: 143;
    COLOR_RGB2YUV_UYVY: 143;
    COLOR_RGB2YUV_Y422: 143;
    COLOR_RGB2YUV_YUNV: 147;
    COLOR_RGB2YUV_YUY2: 147;
    COLOR_RGB2YUV_YUYV: 147;
    COLOR_RGB2YUV_YV12: 131;
    COLOR_RGB2YUV_YVYU: 149;
    COLOR_RGBA2BGR: 3;
    COLOR_RGBA2BGR555: 27;
    COLOR_RGBA2BGR565: 17;
    COLOR_RGBA2BGRA: 5;
    COLOR_RGBA2GRAY: 11;
    COLOR_RGBA2RGB: 1;
    COLOR_RGBA2YUV_I420: 129;
    COLOR_RGBA2YUV_IYUV: 129;
    COLOR_RGBA2YUV_UYNV: 145;
    COLOR_RGBA2YUV_UYVY: 145;
    COLOR_RGBA2YUV_Y422: 145;
    COLOR_RGBA2YUV_YUNV: 151;
    COLOR_RGBA2YUV_YUY2: 151;
    COLOR_RGBA2YUV_YUYV: 151;
    COLOR_RGBA2YUV_YV12: 133;
    COLOR_RGBA2YUV_YVYU: 153;
    COLOR_RGBA2mRGBA: 125;
    COLOR_SPACE_ADOBE_RGB: 2;
    COLOR_SPACE_ADOBE_RGBL: 3;
    COLOR_SPACE_APPLE_RGB: 10;
    COLOR_SPACE_APPLE_RGBL: 11;
    COLOR_SPACE_DCI_P3_RGB: 8;
    COLOR_SPACE_DCI_P3_RGBL: 9;
    COLOR_SPACE_LAB_A_10: 33;
    COLOR_SPACE_LAB_A_2: 32;
    COLOR_SPACE_LAB_D50_10: 31;
    COLOR_SPACE_LAB_D50_2: 29;
    COLOR_SPACE_LAB_D55_10: 35;
    COLOR_SPACE_LAB_D55_2: 34;
    COLOR_SPACE_LAB_D65_10: 30;
    COLOR_SPACE_LAB_D65_2: 28;
    COLOR_SPACE_LAB_D75_10: 37;
    COLOR_SPACE_LAB_D75_2: 36;
    COLOR_SPACE_LAB_E_10: 39;
    COLOR_SPACE_LAB_E_2: 38;
    COLOR_SPACE_PRO_PHOTO_RGB: 6;
    COLOR_SPACE_PRO_PHOTO_RGBL: 7;
    COLOR_SPACE_REC_2020_RGB: 14;
    COLOR_SPACE_REC_2020_RGBL: 15;
    COLOR_SPACE_REC_709_RGB: 12;
    COLOR_SPACE_REC_709_RGBL: 13;
    COLOR_SPACE_SRGB: 0;
    COLOR_SPACE_SRGBL: 1;
    COLOR_SPACE_WIDE_GAMUT_RGB: 4;
    COLOR_SPACE_WIDE_GAMUT_RGBL: 5;
    COLOR_SPACE_XYZ_A_10: 21;
    COLOR_SPACE_XYZ_A_2: 20;
    COLOR_SPACE_XYZ_D50_10: 19;
    COLOR_SPACE_XYZ_D50_2: 17;
    COLOR_SPACE_XYZ_D55_10: 23;
    COLOR_SPACE_XYZ_D55_2: 22;
    COLOR_SPACE_XYZ_D65_10: 18;
    COLOR_SPACE_XYZ_D65_2: 16;
    COLOR_SPACE_XYZ_D75_10: 25;
    COLOR_SPACE_XYZ_D75_2: 24;
    COLOR_SPACE_XYZ_E_10: 27;
    COLOR_SPACE_XYZ_E_2: 26;
    COLOR_XYZ2BGR: 34;
    COLOR_XYZ2RGB: 35;
    COLOR_YCrCb2BGR: 38;
    COLOR_YCrCb2RGB: 39;
    COLOR_YUV2BGR: 84;
    COLOR_YUV2BGRA_I420: 105;
    COLOR_YUV2BGRA_IYUV: 105;
    COLOR_YUV2BGRA_NV12: 95;
    COLOR_YUV2BGRA_NV21: 97;
    COLOR_YUV2BGRA_UYNV: 112;
    COLOR_YUV2BGRA_UYVY: 112;
    COLOR_YUV2BGRA_Y422: 112;
    COLOR_YUV2BGRA_YUNV: 120;
    COLOR_YUV2BGRA_YUY2: 120;
    COLOR_YUV2BGRA_YUYV: 120;
    COLOR_YUV2BGRA_YV12: 103;
    COLOR_YUV2BGRA_YVYU: 122;
    COLOR_YUV2BGR_I420: 101;
    COLOR_YUV2BGR_IYUV: 101;
    COLOR_YUV2BGR_NV12: 91;
    COLOR_YUV2BGR_NV21: 93;
    COLOR_YUV2BGR_UYNV: 108;
    COLOR_YUV2BGR_UYVY: 108;
    COLOR_YUV2BGR_Y422: 108;
    COLOR_YUV2BGR_YUNV: 116;
    COLOR_YUV2BGR_YUY2: 116;
    COLOR_YUV2BGR_YUYV: 116;
    COLOR_YUV2BGR_YV12: 99;
    COLOR_YUV2BGR_YVYU: 118;
    COLOR_YUV2GRAY_420: 106;
    COLOR_YUV2GRAY_I420: 106;
    COLOR_YUV2GRAY_IYUV: 106;
    COLOR_YUV2GRAY_NV12: 106;
    COLOR_YUV2GRAY_NV21: 106;
    COLOR_YUV2GRAY_UYNV: 123;
    COLOR_YUV2GRAY_UYVY: 123;
    COLOR_YUV2GRAY_Y422: 123;
    COLOR_YUV2GRAY_YUNV: 124;
    COLOR_YUV2GRAY_YUY2: 124;
    COLOR_YUV2GRAY_YUYV: 124;
    COLOR_YUV2GRAY_YV12: 106;
    COLOR_YUV2GRAY_YVYU: 124;
    COLOR_YUV2RGB: 85;
    COLOR_YUV2RGBA_I420: 104;
    COLOR_YUV2RGBA_IYUV: 104;
    COLOR_YUV2RGBA_NV12: 94;
    COLOR_YUV2RGBA_NV21: 96;
    COLOR_YUV2RGBA_UYNV: 111;
    COLOR_YUV2RGBA_UYVY: 111;
    COLOR_YUV2RGBA_Y422: 111;
    COLOR_YUV2RGBA_YUNV: 119;
    COLOR_YUV2RGBA_YUY2: 119;
    COLOR_YUV2RGBA_YUYV: 119;
    COLOR_YUV2RGBA_YV12: 102;
    COLOR_YUV2RGBA_YVYU: 121;
    COLOR_YUV2RGB_I420: 100;
    COLOR_YUV2RGB_IYUV: 100;
    COLOR_YUV2RGB_NV12: 90;
    COLOR_YUV2RGB_NV21: 92;
    COLOR_YUV2RGB_UYNV: 107;
    COLOR_YUV2RGB_UYVY: 107;
    COLOR_YUV2RGB_Y422: 107;
    COLOR_YUV2RGB_YUNV: 115;
    COLOR_YUV2RGB_YUY2: 115;
    COLOR_YUV2RGB_YUYV: 115;
    COLOR_YUV2RGB_YV12: 98;
    COLOR_YUV2RGB_YVYU: 117;
    COLOR_YUV420p2BGR: 99;
    COLOR_YUV420p2BGRA: 103;
    COLOR_YUV420p2GRAY: 106;
    COLOR_YUV420p2RGB: 98;
    COLOR_YUV420p2RGBA: 102;
    COLOR_YUV420sp2BGR: 93;
    COLOR_YUV420sp2BGRA: 97;
    COLOR_YUV420sp2GRAY: 106;
    COLOR_YUV420sp2RGB: 92;
    COLOR_YUV420sp2RGBA: 96;
    COLOR_mRGBA2RGBA: 126;
    THRESH_BINARY: 0;
    THRESH_BINARY_INV: 1;
    THRESH_DRYRUN: 128;
    THRESH_MASK: 7;
    THRESH_OTSU: 8;
    THRESH_TOZERO: 3;
    THRESH_TOZERO_INV: 4;
    THRESH_TRIANGLE: 16;
    THRESH_TRUNC: 2;
    MORPH_BLACKHAT: 6;
    MORPH_CLOSE: 3;
    MORPH_CROSS: 1;
    MORPH_DIAMOND: 3;
    MORPH_DILATE: 1;
    MORPH_ELLIPSE: 2;
    MORPH_ERODE: 0;
    MORPH_GRADIENT: 4;
    MORPH_HITMISS: 7;
    MORPH_OPEN: 2;
    MORPH_RECT: 0;
    MORPH_TOPHAT: 5;
    RETR_CCOMP: 2;
    RETR_EXTERNAL: 0;
    RETR_FLOODFILL: 4;
    RETR_LIST: 1;
    RETR_TREE: 3;
    CHAIN_APPROX_NONE: 1;
    CHAIN_APPROX_SIMPLE: 2;
    CHAIN_APPROX_TC89_KCOS: 4;
    CHAIN_APPROX_TC89_L1: 3;
    INTER_AREA: 3;
    INTER_CUBIC: 2;
    INTER_LANCZOS4: 4;
    INTER_LINEAR: 1;
    INTER_LINEAR_EXACT: 5;
    INTER_NEAREST: 0;
    INTER_NEAREST_EXACT: 6;
    BORDER_CONSTANT: 0;
    BORDER_DEFAULT: 4;
    BORDER_ISOLATED: 16;
    BORDER_REFLECT: 2;
    BORDER_REFLECT101: 4;
    BORDER_REFLECT_101: 4;
    BORDER_REPLICATE: 1;
    BORDER_TRANSPARENT: 5;
    BORDER_WRAP: 3;
    TM_CCOEFF: 4;
    TM_CCOEFF_NORMED: 5;
    TM_CCORR: 2;
    TM_CCORR_NORMED: 3;
    TM_SQDIFF: 0;
    TM_SQDIFF_NORMED: 1;
    NORM_HAMMING: 6;
    NORM_HAMMING2: 7;
    NORM_INF: 1;
    NORM_L1: 2;
    NORM_L2: 4;
    NORM_L2SQR: 5;
    NORM_MINMAX: 32;
    NORM_RELATIVE: 8;
    CMP_EQ: 0;
    CMP_GE: 2;
    CMP_GT: 1;
    CMP_LE: 4;
    CMP_LT: 3;
    CMP_NE: 5;
    FONT_HERSHEY_COMPLEX: 3;
    FONT_HERSHEY_COMPLEX_SMALL: 5;
    FONT_HERSHEY_DUPLEX: 2;
    FONT_HERSHEY_PLAIN: 1;
    FONT_HERSHEY_SCRIPT_COMPLEX: 7;
    FONT_HERSHEY_SCRIPT_SIMPLEX: 6;
    FONT_HERSHEY_SIMPLEX: 0;
    FONT_HERSHEY_TRIPLEX: 4;
    LINE_4: 4;
    LINE_8: 8;
    LINE_AA: 16;
    COLORMAP_AUTUMN: 0;
    COLORMAP_BONE: 1;
    COLORMAP_CIVIDIS: 17;
    COLORMAP_COOL: 8;
    COLORMAP_DEEPGREEN: 21;
    COLORMAP_HOT: 11;
    COLORMAP_HSV: 9;
    COLORMAP_INFERNO: 14;
    COLORMAP_JET: 2;
    COLORMAP_MAGMA: 13;
    COLORMAP_OCEAN: 5;
    COLORMAP_PARULA: 12;
    COLORMAP_PINK: 10;
    COLORMAP_PLASMA: 15;
    COLORMAP_RAINBOW: 4;
    COLORMAP_SPRING: 7;
    COLORMAP_SUMMER: 6;
    COLORMAP_TURBO: 20;
    COLORMAP_TWILIGHT: 18;
    COLORMAP_TWILIGHT_SHIFTED: 19;
    COLORMAP_VIRIDIS: 16;
    COLORMAP_WINTER: 3;
    DIST_C: 3;
    DIST_FAIR: 5;
    DIST_HUBER: 7;
    DIST_L1: 1;
    DIST_L12: 4;
    DIST_L2: 2;
    DIST_LABEL_CCOMP: 0;
    DIST_LABEL_PIXEL: 1;
    DIST_MASK_3: 3;
    DIST_MASK_5: 5;
    DIST_MASK_PRECISE: 0;
    DIST_USER: -1;
    DIST_WELSCH: 6;
    HOUGH_GRADIENT: 3;
    HOUGH_GRADIENT_ALT: 4;
    HOUGH_MULTI_SCALE: 2;
    HOUGH_PROBABILISTIC: 1;
    HOUGH_STANDARD: 0;
    KMEANS_PP_CENTERS: 2;
    KMEANS_RANDOM_CENTERS: 0;
    KMEANS_USE_INITIAL_LABELS: 1;
    HISTCMP_BHATTACHARYYA: 3;
    HISTCMP_CHISQR: 1;
    HISTCMP_CHISQR_ALT: 4;
    HISTCMP_CORREL: 0;
    HISTCMP_HELLINGER: 3;
    HISTCMP_INTERSECT: 2;
    HISTCMP_KL_DIV: 5;
    CALIB_CB_ACCURACY: 32;
    CALIB_CB_ADAPTIVE_THRESH: 1;
    CALIB_CB_ASYMMETRIC_GRID: 2;
    CALIB_CB_CLUSTERING: 4;
    CALIB_CB_EXHAUSTIVE: 16;
    CALIB_CB_FAST_CHECK: 8;
    CALIB_CB_FILTER_QUADS: 4;
    CALIB_CB_LARGER: 64;
    CALIB_CB_MARKER: 128;
    CALIB_CB_NORMALIZE_IMAGE: 2;
    CALIB_CB_PLAIN: 256;
    CALIB_CB_SYMMETRIC_GRID: 1;
    CALIB_CHECK_COND: 16777216;
    CALIB_DISABLE_SCHUR_COMPLEMENT: 262144;
    CALIB_FIX_ASPECT_RATIO: 2;
    CALIB_FIX_FOCAL_LENGTH: 16;
    CALIB_FIX_INTRINSIC: 256;
    CALIB_FIX_K1: 32;
    CALIB_FIX_K2: 64;
    CALIB_FIX_K3: 128;
    CALIB_FIX_K4: 2048;
    CALIB_FIX_K5: 4096;
    CALIB_FIX_K6: 8192;
    CALIB_FIX_PRINCIPAL_POINT: 4;
    CALIB_FIX_S1_S2_S3_S4: 65536;
    CALIB_FIX_SKEW: 33554432;
    CALIB_FIX_TANGENT_DIST: 2097152;
    CALIB_FIX_TAUX_TAUY: 524288;
    CALIB_HAND_EYE_ANDREFF: 3;
    CALIB_HAND_EYE_DANIILIDIS: 4;
    CALIB_HAND_EYE_HORAUD: 2;
    CALIB_HAND_EYE_PARK: 1;
    CALIB_HAND_EYE_TSAI: 0;
    CALIB_MODEL_FISHEYE: 1;
    CALIB_MODEL_PINHOLE: 0;
    CALIB_RATIONAL_MODEL: 16384;
    CALIB_RECOMPUTE_EXTRINSIC: 8388608;
    CALIB_ROBOT_WORLD_HAND_EYE_LI: 1;
    CALIB_ROBOT_WORLD_HAND_EYE_SHAH: 0;
    CALIB_SAME_FOCAL_LENGTH: 512;
    CALIB_STEREO_REGISTRATION: 67108864;
    CALIB_THIN_PRISM_MODEL: 32768;
    CALIB_TILTED_MODEL: 262144;
    CALIB_USE_EXTRINSIC_GUESS: 4194304;
    CALIB_USE_INTRINSIC_GUESS: 1;
    CALIB_USE_LU: 131072;
    CALIB_USE_QR: 1048576;
    CALIB_ZERO_DISPARITY: 1024;
    CALIB_ZERO_TANGENT_DIST: 8;
    WARP_FILL_OUTLIERS: 8;
    WARP_INVERSE_MAP: 16;
    WARP_POLAR_LINEAR: 0;
    WARP_POLAR_LOG: 256;
    WARP_RELATIVE_MAP: 32;
    INPAINT_NS: 0;
    INPAINT_TELEA: 1;
    RANSAC: 8;
    CONTOURS_MATCH_I1: 1;
    CONTOURS_MATCH_I2: 2;
    CONTOURS_MATCH_I3: 3;
    LMEDS: 4;
    RHO: 16;
    IMREAD_UNCHANGED: -1;
    IMREAD_GRAYSCALE: 0;
    IMREAD_COLOR: 1;
    IMREAD_ANYDEPTH: 2;
    IMREAD_ANYCOLOR: 4;
    IMREAD_IGNORE_ORIENTATION: 128;
    ADAPTIVE_THRESH_MEAN_C: 0;
    ADAPTIVE_THRESH_GAUSSIAN_C: 1;
    DECOMP_LU: 0;
    DECOMP_SVD: 1;
    DECOMP_EIG: 2;
    DECOMP_CHOLESKY: 3;
    DECOMP_QR: 4;
    DECOMP_NORMAL: 16;
    TERM_CRITERIA_COUNT: 1;
    TERM_CRITERIA_MAX_ITER: 1;
    TERM_CRITERIA_EPS: 2;
    CvError: typeof CvError;
    Mat: typeof Mat;
    MatVector: typeof MatVector;
    CLAHE: typeof CLAHE;
    createCLAHE: typeof createCLAHE;
    Size: typeof Size;
    Point: typeof Point;
    Rect: typeof Rect;
    Scalar: typeof Scalar;
    Circle: typeof Circle;
    RotatedRect: typeof RotatedRect;
    available: typeof available;
    count: typeof count;
    releaseAll: typeof releaseAll;
    ops: typeof ops;
    op: typeof op;
    zeros: typeof zeros;
    ones: typeof ones;
    full: typeof full;
    fromData: typeof fromData;
    fromImage: typeof fromImage;
    imread: typeof imread;
    imdecode: typeof imdecode;
    imencode: typeof imencode;
    imwrite: typeof imwrite;
    matFromArray: typeof matFromArray;
    matFromImageData: typeof matFromImageData;
    CV_MAKETYPE: typeof CV_MAKETYPE;
    CV_8UC: typeof CV_8UC;
    CV_8SC: typeof CV_8SC;
    CV_16UC: typeof CV_16UC;
    CV_16SC: typeof CV_16SC;
    CV_32SC: typeof CV_32SC;
    CV_32FC: typeof CV_32FC;
    CV_64FC: typeof CV_64FC;
    cvtColor: typeof cvtColor;
    inRange: typeof inRange;
    convertTo: typeof convertTo;
    split: typeof split;
    merge: typeof merge;
    LUT: typeof LUT;
    applyColorMap: typeof applyColorMap;
    equalizeHist: typeof equalizeHist;
    clahe: typeof clahe;
    threshold: typeof threshold;
    adaptiveThreshold: typeof adaptiveThreshold;
    blur: typeof blur;
    boxFilter: typeof boxFilter;
    GaussianBlur: typeof GaussianBlur;
    medianBlur: typeof medianBlur;
    bilateralFilter: typeof bilateralFilter;
    filter2D: typeof filter2D;
    Sobel: typeof Sobel;
    Scharr: typeof Scharr;
    Laplacian: typeof Laplacian;
    Canny: typeof Canny;
    getStructuringElement: typeof getStructuringElement;
    erode: typeof erode;
    dilate: typeof dilate;
    morphologyEx: typeof morphologyEx;
    connectedComponents: typeof connectedComponents;
    resize: typeof resize;
    crop: typeof crop;
    flip: typeof flip;
    rotate: typeof rotate;
    transpose: typeof transpose;
    warpAffine: typeof warpAffine;
    warpPerspective: typeof warpPerspective;
    getRotationMatrix2D: typeof getRotationMatrix2D;
    getPerspectiveTransform: typeof getPerspectiveTransform;
    copyMakeBorder: typeof copyMakeBorder;
    hconcat: typeof hconcat;
    vconcat: typeof vconcat;
    add: typeof add;
    subtract: typeof subtract;
    multiply: typeof multiply;
    divide: typeof divide;
    absdiff: typeof absdiff;
    bitwiseAnd: typeof bitwise_and;
    bitwiseOr: typeof bitwise_or;
    bitwiseXor: typeof bitwise_xor;
    bitwiseNot: typeof bitwise_not;
    bitwise_and: typeof bitwise_and;
    bitwise_or: typeof bitwise_or;
    bitwise_xor: typeof bitwise_xor;
    bitwise_not: typeof bitwise_not;
    addWeighted: typeof addWeighted;
    compare: typeof compare;
    min: typeof min;
    max: typeof max;
    normalize: typeof normalize;
    meanStdDev: typeof meanStdDev;
    mean: typeof mean;
    minMaxLoc: typeof minMaxLoc;
    countNonZero: typeof countNonZero;
    sum: typeof sum;
    norm: typeof norm;
    findContours: typeof findContours;
    contourPoints: typeof contourPoints;
    contourArea: typeof contourArea;
    arcLength: typeof arcLength;
    boundingRect: typeof boundingRect;
    minAreaRect: typeof minAreaRect;
    convexHull: typeof convexHull;
    approxPolyDP: typeof approxPolyDP;
    isContourConvex: typeof isContourConvex;
    contourPointCount: typeof contourPointCount;
    contourCircularity: typeof contourCircularity;
    moments: typeof moments;
    HuMoments: typeof HuMoments;
    matchTemplate: typeof matchTemplate;
    calcHist: typeof calcHist;
    line: typeof line;
    rectangle: typeof rectangle;
    circle: typeof circle;
    ellipse: typeof ellipse;
    polylines: typeof polylines;
    putText: typeof putText;
    TermCriteria: typeof TermCriteria;
    sqrt: typeof sqrt;
    log: typeof log;
    exp: typeof exp;
    pow: typeof pow;
    magnitude: typeof magnitude;
    phase: typeof phase;
    cartToPolar: typeof cartToPolar;
    polarToCart: typeof polarToCart;
    pyrDown: typeof pyrDown;
    pyrUp: typeof pyrUp;
    reduce: typeof reduce;
    findNonZero: typeof findNonZero;
    integral: typeof integral;
    getAffineTransform: typeof getAffineTransform;
    HoughCircles: typeof HoughCircles;
    HoughLinesP: typeof HoughLinesP;
    distanceTransform: typeof distanceTransform;
    floodFill: typeof floodFill;
    watershed: typeof watershed;
    kmeans: typeof kmeans;
    cornerHarris: typeof cornerHarris;
    goodFeaturesToTrack: typeof goodFeaturesToTrack;
    drawContours: typeof drawContours;
    minEnclosingCircle: typeof minEnclosingCircle;
    fitEllipse: typeof fitEllipse;
    fitLine: typeof fitLine;
    matchShapes: typeof matchShapes;
    convexityDefects: typeof convexityDefects;
    compareHist: typeof compareHist;
    calcBackProject: typeof calcBackProject;
    findHomography: typeof findHomography;
    undistort: typeof undistort;
    remap: typeof remap;
    calcOpticalFlowFarneback: typeof calcOpticalFlowFarneback;
    connectedComponentsWithStats: typeof connectedComponentsWithStats;
    fastNlMeansDenoising: typeof fastNlMeansDenoising;
    inpaint: typeof inpaint;
};
export default cv;
