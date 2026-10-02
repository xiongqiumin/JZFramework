// 'cv' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型/值分别转发到两处 ——
//   · **类型**  <- ./index  = cv/types/index.d.ts (本包权威类型定义)
//   · **值**    <- ../index = cv/index.ts        (运行期实现, 编译成 dist/index.js)
//
// ⚠ TS7 + moduleResolution:bundler 下, ambient 内 `export * from './x'` 不会转发**具名导出**
//   (只转发 default), 故这里一律**显式具名再导出**(与 computeruse / buffer 包同一个坑、同一个写法)。
//   ⛔ 本包加新导出时, 必须同时在下面两张表里补名, 否则 `import { X } from 'cv'` 报 TS2305。
declare module 'cv' {
    export type {
        MatLike, MatInfo, Size2, Point2, RectArg, ColorArg, KernelArg,
        MatType, ColorCode, Interp, BorderType, ThreshType, MorphOp, Colormap,
        RetrMode, ChainMethod, TmMethod, NormType, CmpOp, FlipHow, RotateCode,
        Contour, ComponentsResult, MinMaxLocResult, MeanStdDevResult, MomentsResult,
        MatchTemplateResult, HistResult, ThresholdResult, CvBackend,
    } from './index';
    export {
        CvError, Mat,
        available, count, releaseAll, ops, op,
        zeros, ones, full, fromData, imread, imdecode, imencode,
        cvtColor, inRange, convertTo, split, merge, lut, applyColorMap, equalizeHist, clahe,
        threshold, adaptiveThreshold,
        blur, boxFilter, gaussianBlur, medianBlur, bilateralFilter, filter2D,
        sobel, scharr, laplacian, canny,
        getStructuringElement, erode, dilate, morphologyEx, connectedComponents,
        resize, crop, flip, rotate, transpose, warpAffine, warpPerspective,
        getRotationMatrix2D, getPerspectiveTransform, copyMakeBorder, hconcat, vconcat,
        add, subtract, multiply, divide, absdiff,
        bitwiseAnd, bitwiseOr, bitwiseXor, bitwiseNot, addWeighted, compare, min, max, normalize,
        meanStdDev, minMaxLoc, countNonZero, sum, norm,
        findContours, moments, matchTemplate, calcHist,
        line, rectangle, circle, ellipse, polylines, putText,
    } from '../index';
    export { default } from '../index';
}
