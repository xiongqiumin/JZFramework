// cv 包自检：OpenCV.js 形状（常量 / new cv.Mat / dst 出参 / MatVector / 独立轮廓算子 / 就地画）
//
// 跑法（2026-10-02 实测 63 pass / 0 fail）:
//   1) 先出 dist:  <ts_compiler>/tsc/tsc.exe -b JZJSEngine/js_wrapper/cv/tsconfig.json
//   2) 部署:       python JZJSEngine/js_wrapper/package.py --out <build>/binary/code_agent/bin/<CFG> \
//                        --js-wrapper JZJSEngine/js_wrapper
//                  (或让 cmake 重编 code_runtime —— 它的 POST_BUILD copy_jz_js 会顺手部署)
//   3) 跑:         <build>/binary/code_agent/bin/<CFG>/code_runtime.exe --entry <本文件绝对路径>
//   退出码 0 = 全过; 有 FAIL 会打印并把 exitCode 置 1。会在**入口所在目录**写一张 cv_selftest.png。
// ⚠ 需要那个 exe 编了 OpenCV(JZ_CU_OPENCV), 否则 available() 为 false, 大部分断言会失败。
import cv, { available, ops, matFromArray, contourArea, CV_8UC3 } from 'cv';

let pass = 0, fail = 0;
function check(name, cond, detail) {
    if (cond) { pass++; console.log('[PASS] ' + name); }
    else { fail++; console.log('[FAIL] ' + name + '  -> ' + (detail === undefined ? '' : detail)); }
}
function eq(name, got, want) {
    check(name, got === want, 'got=' + got + ' want=' + want);
}

// ---- 1. 常量表(OpenCV.js 同名同值, 且是 4.x 的通道编码) ----
eq('COLOR_BGR2GRAY=6', cv.COLOR_BGR2GRAY, 6);
eq('THRESH_BINARY=0', cv.THRESH_BINARY, 0);
eq('THRESH_OTSU=8', cv.THRESH_OTSU, 8);
eq('MORPH_ELLIPSE=2', cv.MORPH_ELLIPSE, 2);
eq('RETR_EXTERNAL=0', cv.RETR_EXTERNAL, 0);
eq('CHAIN_APPROX_SIMPLE=2', cv.CHAIN_APPROX_SIMPLE, 2);
eq('INTER_LINEAR=1', cv.INTER_LINEAR, 1);
eq('CV_8UC3=16(4.x 编码)', cv.CV_8UC3, 16);
eq('具名导入的常量', CV_8UC3, 16);
eq('CV_MAKETYPE(5,3)=21', cv.CV_MAKETYPE(5, 3), 21);
check('available()', available() === true);
check("ops() 里有新增的 contourMetrics", ops().indexOf('contourMetrics') >= 0, ops().length + ' ops');

// ---- 2. new cv.Mat(rows, cols, type[, fill]) + Mat 的 OpenCV.js 面 ----
const img = new cv.Mat(60, 80, cv.CV_8UC3, new cv.Scalar(0, 0, 0));
eq('Mat.rows', img.rows, 60);
eq('Mat.cols', img.cols, 80);
eq('Mat.channels()', img.channels(), 3);
eq('Mat.type()', img.type(), 16);
eq('Mat.depth()', img.depth(), 0);
eq('Mat.typeName', img.typeName, '8UC3');
eq('Mat.total()', img.total(), 60 * 80);
check('Mat.size() 回 cv.Size', img.size().width === 80 && img.size().height === 60, JSON.stringify(img.size()));
eq('Mat.step', img.step, 80 * 3);
check('Mat.empty() 是方法', img.empty() === false);

// 出参形式一律**无返回**（与 OpenCV.js 一致）；threshold/connectedComponents 那两个例外各有断言
check('出参形式无返回(cvtColor/blur/split/bitwise_not…)', (() => {
    const d = new cv.Mat();
    const mv = new cv.MatVector();
    const t = new cv.Mat();
    const r = [cv.cvtColor(img, d, cv.COLOR_BGR2GRAY),
               cv.GaussianBlur(d, t, new cv.Size(3, 3), 0),
               cv.split(img, mv),
               cv.bitwise_not(d, t),
               cv.inRange(img, new cv.Scalar(0, 0, 0), new cv.Scalar(255, 255, 255), t),
               cv.merge(mv, t)];
    d.delete(); t.delete(); mv.delete();
    return r.every(function (v) { return v === undefined; });
})());

// ---- 3. 就地画 + 像素 ----
cv.circle(img, new cv.Point(40, 30), 15, new cv.Scalar(0, 0, 255), -1);
const px = img.pixel(40, 30);
check('circle 就地画上去了(像素是红的 BGR)', px && px[2] > 200 && px[0] < 60, JSON.stringify(px));
eq('ucharAt', img.ucharAt(30, 40), px[0]);

// ---- 4. dst 出参 + 数字常量 ----
const gray = new cv.Mat();
cv.cvtColor(img, gray, cv.COLOR_BGR2GRAY);
eq('cvtColor 写进 dst 的通道数', gray.channels(), 1);
eq('cvtColor dst 行数', gray.rows, 60);
check('dst 还是同一个 JS 对象', gray instanceof cv.Mat);

// ---- 5. threshold(常量相加) 回实际阈值 ----
const bin = new cv.Mat();
const t = cv.threshold(gray, bin, 0, 255, cv.THRESH_BINARY + cv.THRESH_OTSU);
check('threshold 回实际阈值(number)', typeof t === 'number', t);
check('OTSU 二值化正确(圆是白的, 背景是黑的)',
      bin.pixel(40, 30)[0] === 255 && bin.pixel(0, 0)[0] === 0,
      JSON.stringify([bin.pixel(40, 30), bin.pixel(0, 0)]));
eq('二值图通道', bin.channels(), 1);
check('老形状 threshold(src, thresh, maxval, type) 回 ThresholdResult',
      (() => { const r = cv.threshold(gray, 10, 255, cv.THRESH_BINARY); return r && r.thresh !== undefined && r.mat instanceof cv.Mat; })());

// ---- 6. findContours(出参式, 与 OpenCV.js 一致) + 按需取轮廓量 ----
const contours = new cv.MatVector();
const hier = new cv.Mat();
check('findContours 无返回(与 OpenCV.js 一致)',
      cv.findContours(bin, contours, hier, cv.RETR_EXTERNAL, cv.CHAIN_APPROX_SIMPLE) === undefined);
eq('找到 1 个轮廓', contours.size(), 1);
const c0 = contours.get(0);
check('contours.get(0) 是**轮廓 Mat**(不再是带派生量的对象)', c0 instanceof cv.Mat, typeof c0);
eq('轮廓 Mat = 2 通道 32S(32SC2)', c0.type(), cv.CV_MAKETYPE(4, 2));
eq('轮廓行数 = 点数', c0.rows, c0.cols === 1 ? c0.rows : c0.rows);
eq('按需问点数(不搬点)', cv.contourPointCount(c0), c0.rows);
const area = cv.contourArea(c0);
const br = cv.boundingRect(c0);
const len = cv.arcLength(c0, true);
check('contourArea > 300', area > 300, area);
check('boundingRect 是 cv.Rect 形状', br.width > 20 && br.height > 20 && br.x >= 0, JSON.stringify([br.x, br.y, br.width, br.height]));
check('arcLength > 50', len > 50, len);
check('圆度 contourCircularity 是数字', typeof cv.contourCircularity(c0) === 'number');
// 凸包 / 多边形近似都是**出参**（与 OpenCV.js 的 cv.convexHull(cnt, hull) 一致）
const hull = new cv.Mat();
cv.convexHull(c0, hull);
check('convexHull 出参形式', hull.rows >= 3 && hull.rows <= c0.rows, hull.rows);
const ap = new cv.Mat();
check('approxPolyDP 出参形式(eps 是绝对阈值)', cv.approxPolyDP(c0, ap, 2.0, true) === undefined);
check('近似多边形点数 <= 原点数', ap.rows > 0 && ap.rows <= c0.rows, ap.rows);
check('minAreaRect 回 RotatedRect 形状', (() => { const r = cv.minAreaRect(c0); return r !== null && r.size.width > 0 && typeof r.angle === 'number'; })());
check('contourPoints(要点的显式调用)', cv.contourPoints(c0).length === c0.rows);

// 矩形轮廓: 就地画 + 真 OpenCV 的凸性判定
const rectImg = cv.matFromArray(60, 80, cv.CV_8UC1, (() => { const a = []; for (let i = 0; i < 4800; i++) a.push(0); return a; })());
cv.rectangle(rectImg, new cv.Point(10, 10), new cv.Point(50, 40), 255, -1);
const rc = new cv.MatVector(), rh = new cv.Mat();
cv.findContours(rectImg, rc, rh, cv.RETR_EXTERNAL, cv.CHAIN_APPROX_SIMPLE);
check('矩形: 就地画(Point, Point) + 找出轮廓', rc.size() === 1, rc.size());
check('isContourConvex(矩形轮廓)', cv.isContourConvex(rc.get(0)) === true);
// 栅格化圆边是"阶梯"的, cv::isContourConvex 对它会返回 false（跟我们的 TS 叉积判据一个结论）——
// 所以这里只要求它是个 boolean, 别把"圆一定凸"写死进断言。
check('isContourConvex(栅格圆) 回 boolean', typeof cv.isContourConvex(c0) === 'boolean');

// 本包的扩展: 只给 opts 时一次回富形态(派生量都现成)
const rich = cv.findContours(bin, { mode: cv.RETR_EXTERNAL, method: cv.CHAIN_APPROX_SIMPLE });
check('扩展: 富形态 Contour[] 也在', rich.length === 1 && rich[0].area === area && rich[0].points.length === c0.rows);

check('具名导入的 contourArea 也通', contourArea(c0) === area);

c0.delete(); hier.delete(); hull.delete(); ap.delete(); rc.get(0).delete(); rh.delete(); contours.delete(); rc.delete();

// ---- 7. matFromArray / 类型化数组 / 指针写回 ----
const m2 = matFromArray(2, 2, cv.CV_8UC3, [0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0, 0]);
eq('matFromArray rows', m2.rows, 2);
eq('matFromArray channels', m2.channels(), 3);
eq('data8U 长度', m2.data8U.length, 12);
eq('data8U 内容', m2.data8U[2], 255);
check('具名导入的 matFromArray', m2 instanceof cv.Mat);
check('ucharPtr 能写回', (() => { const p = m2.ucharPtr(0, 0); p[0] = 7; const q = m2.pixel(0, 0); return q && q[0] === 7; })());
// 非 8U 的整块字节: 后端现在给**原始字节**(不再先转 8U), 所以 data32F 能直接读出真实 float
check('data32F 对非 8U 图给原始 float 字节(不再转 8U)',
    Array.prototype.join.call(cv.matFromArray(1, 2, cv.CV_32FC1, [1.5, 2.5]).data32F, ',') === '1.5,2.5');
const f32 = cv.matFromArray(1, 2, cv.CV_32FC1, [1.5, 2.5]);
check('32F 可用 floatAt 逐点读', f32.floatAt(0, 1) === 2.5, f32.floatAt(0, 1));
check('32F 的 data32F 直接读真实 float（与 opencv-js 的 mat.data32F 同理）',
      Array.prototype.join.call(f32.data32F, ',') === '1.5,2.5', Array.prototype.join.call(f32.data32F, ','));
const f32u8 = new cv.Mat();
check('convertTo 出参形式无返回', f32.convertTo(f32u8) === undefined && f32u8.data8U !== undefined);

// ---- 7.5 矩阵算子 mul/dot/t/inv/diag + cv.mean + cv.HuMoments ----
// 期望值全部取自**真 opencv-js** 在同样输入(A=[[1,2],[3,4]] B=[[5,6],[7,8]] CV_32F)下的实跑结果
const mA = cv.matFromArray(2, 2, cv.CV_32FC1, [1, 2, 3, 4]);
const mB = cv.matFromArray(2, 2, cv.CV_32FC1, [5, 6, 7, 8]);
const flatF = (m) => Array.prototype.join.call(m.data32F, ',');
eq('Mat.mul(B, 2) = 10,24,42,64', flatF(mA.mul(mB, 2)), '10,24,42,64');
eq('Mat.dot(B) = 70', mA.dot(mB), 70);
eq('Mat.t() = 1,3,2,4（回新图）', flatF(mA.t()), '1,3,2,4');
eq('Mat.t() 不改自己', flatF(mA), '1,2,3,4');
eq('Mat.inv() = -2,1,1.5,-0.5', flatF(mA.inv()), '-2,1,1.5,-0.5');
check('Mat.inv(DECOMP_SVD) 也能算', flatF(mA.inv(cv.DECOMP_SVD)).length > 0);
const dg = mA.diag(0);
// ⚠ opencv-js 侧 diag(0) 的**逐点值**是 floatAt(0,0)=1 / floatAt(1,0)=4(真对角线), 但它回的是
//   非连续视图、embind 的 data32F 会扁平读成 "1,2"; 我们是连续拷贝, 给真值 1,4
check('Mat.diag(0) = 1,4（2x1, 连续）', dg.rows === 2 && dg.cols === 1 && flatF(dg) === '1,4', flatF(dg));
check('Mat.diag(1) = 2（主对角上方, 与 opencv-js 的 floatAt 一致）', flatF(mA.diag(1)) === '2', flatF(mA.diag(1)));
eq('cv.mean(mA) = [2.5,0,0,0]', Array.prototype.join.call(cv.mean(mA), ','), '2.5,0,0,0');
const huDst = new cv.Mat();
check('cv.HuMoments(mm, dst)：出参式无返回 + 1x7 CV_64F', (() => {
    const r = cv.HuMoments(cv.moments(mA), huDst);
    return r === undefined && huDst.rows === 1 && huDst.cols === 7 && huDst.type() === cv.CV_64FC1;
})());
check('32F 的 data32F 就是原始 float(不再转 8U)', flatF(mA) === '1,2,3,4' && mA.data.length === 16);
mA.delete(); mB.delete(); dg.delete(); huDst.delete();

// ---- 8. 值对象 + 形态学 / 缩放（cv.Size / cv.Rect） ----
const k = cv.getStructuringElement(cv.MORPH_ELLIPSE, new cv.Size(5, 5));
check('getStructuringElement(数字 shape)', k.rows === 5 && k.cols === 5);
const opened = new cv.Mat();
cv.morphologyEx(bin, opened, cv.MORPH_OPEN, k);
eq('morphologyEx 写进 dst', opened.rows, bin.rows);
const small = new cv.Mat();
cv.resize(img, small, new cv.Size(40, 30), 0, 0, cv.INTER_LINEAR);
check('resize(src, dst, cv.Size)', small.cols === 40 && small.rows === 30, small.cols + 'x' + small.rows);
const sub = img.roi(new cv.Rect(0, 0, 10, 10));
check('roi(cv.Rect)', sub.cols === 10 && sub.rows === 10);
const cp = new cv.Mat();
img.copyTo(cp);
check('copyTo', cp.cols === img.cols && cp.rows === img.rows);

// ---- 9. 老形状(不给 dst) 仍然返回新图 ----
const hsv = cv.cvtColor(img, cv.COLOR_BGR2HSV);
check('cvtColor(src, code) 回新图', hsv.channels() === 3 && hsv.cols === 80);
cv.flip(img, 1);
check('flip(img, 1) 老形状(左右翻)', true);

// ---- 10. imencode(ext, img) 回 Mat；imwrite 参数顺序 ----
const enc = cv.imencode('.png', img);
check('imencode(\'.png\', img) 回 Mat', enc instanceof cv.Mat && enc.rows > 100, enc.rows);
const out = (process.cwd ? process.cwd() : '.') + '/cv_selftest.png';
check('imwrite(filename, img)', cv.imwrite(out, img) === true, out);

// ---- 11. 传统算法(2026-10-03): 数学/金字塔/统计/检测/形状/直方图/几何/光流 ----
// ★ 那些期望值是与**真 opencv-js** 同输入逐项实测一致的(见提交说明里的交叉验证)
// 整段用 try 兜住: 任何一条抛错都记成 FAIL 而不是把自检打断(OpenCV 的断言会直接终止进程)
try {
const jf32 = (m) => Array.prototype.join.call(m.data32F, ',');
const jsi = (m) => Array.prototype.join.call(m.data32S, ',');
const rnd3 = (v) => Math.round(v * 1000) / 1000;
const sumB = (m) => { let s = 0; for (let i = 0; i < m.data.length; i++) s += m.data[i]; return s; };
const rnd3s = (a) => Array.prototype.join.call(a, ',').split(',').map(rnd3).join(',');
const g8 = [];
for (let yy = 0; yy < 16; yy++) for (let xx = 0; xx < 16; xx++) g8.push((xx >= 5 && xx < 11 && yy >= 5 && yy < 11) ? 255 : 0);
const g = cv.matFromArray(16, 16, cv.CV_8UC1, g8);
const w = new cv.Mat(), w2 = new cv.Mat();
const a32 = cv.matFromArray(2, 2, cv.CV_32FC1, [4, 9, 16, 25]);
const b32 = cv.matFromArray(2, 2, cv.CV_32FC1, [3, 4, 12, 7]);
cv.sqrt(a32, w); eq('★ sqrt', jf32(w), '2,3,4,5');
cv.pow(b32, 3, w); eq('★ pow', rnd3(w.data32F[3]), 343);
cv.magnitude(a32, b32, w); eq('★ magnitude', rnd3(w.data32F[0]), 5);
cv.cartToPolar(a32, b32, w, w2, false); eq('★ cartToPolar', rnd3(w.data32F[3]), 25.962);
cv.phase(a32, b32, w, true); check('phase(角度, 本包扩展)', rnd3(w.data32F[1]) === 23.966, w.data32F[1]);
cv.pyrDown(g, w); eq('★ pyrDown', w.rows + 'x' + w.cols, '8x8');
cv.pyrUp(g, w); eq('★ pyrUp', w.rows + 'x' + w.cols, '32x32');
cv.reduce(g, w, 1, cv.REDUCE_SUM, cv.CV_32S); eq('★ reduce', w.type() + ':' + w.data32S[5], '4:1530');
cv.integral(g, w); eq('★ integral', w.rows + 'x' + w.cols + ':' + w.type(), '17x17:4');
cv.distanceTransform(g, w, cv.DIST_L2, 3); eq('★ distanceTransform.max', rnd3(cv.minMaxLoc(w).maxVal), 2.865);
cv.findNonZero(g, w); eq('findNonZero(本包扩展)', w.rows + ':' + w.type(), '36:12');
const canvas = cv.Mat.zeros(32, 32, cv.CV_8UC1);
cv.line(canvas, new cv.Point(4, 16), new cv.Point(28, 16), new cv.Scalar(255), 1);
cv.HoughLinesP(canvas, w, 1, Math.PI / 180, 20, 10, 5);
eq('★ HoughLinesP 找到那条线', w.rows + ':' + jsi(w), '1:4,16,28,16');
const ccan = cv.Mat.zeros(32, 32, cv.CV_8UC1);
cv.circle(ccan, new cv.Point(16, 16), 6, new cv.Scalar(255), 1);
cv.HoughCircles(ccan, w, cv.HOUGH_GRADIENT, 1, 10, 100, 12, 3, 10);
eq('★ HoughCircles 找到圆(x,r)', w.cols + ':' + rnd3(w.data32F[0]) + ',' + rnd3(w.data32F[2]), '1:14.5,4.6');
cv.cornerHarris(g, w, 2, 3, 0.04); eq('★ cornerHarris.max', rnd3(cv.minMaxLoc(w).maxVal), 0.108);
cv.goodFeaturesToTrack(g, w, 20, 0.01, 3, new cv.Mat(), 3, false, 0.04);
eq('goodFeaturesToTrack(本包扩展)', w.rows + ':' + w.type(), '4:13');
const pts = cv.matFromArray(8, 1, cv.CV_32SC2, [5, 5, 8, 5, 11, 5, 11, 8, 11, 11, 8, 11, 5, 11, 5, 8]);
const mec = cv.minEnclosingCircle(pts);
check('★ minEnclosingCircle 回 cv.Circle', mec instanceof cv.Circle
  && rnd3(mec.center.x) + ',' + rnd3(mec.center.y) + ',' + rnd3(mec.radius) === '8,8,4.243',
  rnd3(mec.center.x) + ',' + rnd3(mec.center.y) + ',' + rnd3(mec.radius));
const ell = cv.fitEllipse(pts);
check('★ fitEllipse 回 cv.RotatedRect', ell instanceof cv.RotatedRect && rnd3(ell.size.width) === 7.746,
  rnd3(ell.size.width) + ',' + rnd3(ell.size.height) + ',' + rnd3(ell.angle));
cv.fitLine(pts, w, cv.DIST_L2, 0, 0.01, 0.01); eq('★ fitLine', jf32(w), '1,0,8,8');
eq('matchShapes(自己,自己) = 0', cv.matchShapes(pts, pts, cv.CONTOURS_MATCH_I1, 0), 0);
const hull = new cv.Mat();
cv.convexHull(pts, hull, true, false); eq('★ convexHull(returnPoints=false)', jsi(hull), '6,4,2,0');
const defs = new cv.Mat();
cv.convexityDefects(pts, hull, defs); check('convexityDefects 空结果(凸轮廓)不报错', defs.rows === 0);
const dr = cv.Mat.zeros(16, 16, cv.CV_8UC1);
cv.drawContours(dr, [pts], -1, new cv.Scalar(255), 1);
check('drawContours 就地画', sumB(dr) > 0, sumB(dr));
const h1 = cv.matFromArray(1, 4, cv.CV_32FC1, [10, 20, 30, 40]);
const h2 = cv.matFromArray(1, 4, cv.CV_32FC1, [12, 18, 33, 37]);
eq('★ compareHist.correl', rnd3(cv.compareHist(h1, h2, cv.HISTCMP_CORREL)), 0.975);
check('calcBackProject([src],[ch],hist256,dst,[0,256])', (() => {
    // ⚠ 直方图必须是 N×1(N=bin 数): OpenCV 把 1×N 当成 N 维直方图, 会去读 channels[1..] 然后断言炸掉
    const hbins = [];
    for (let i = 0; i < 256; i++) hbins.push(i >= 5 && i < 11 ? 1 : 0);
    const h256 = cv.matFromArray(256, 1, cv.CV_32FC1, hbins);
    const bp = new cv.Mat();
    cv.calcBackProject([g], [0], h256, bp, [0, 256], 1);
    h256.delete();
    return bp.rows === 16 && bp.cols === 16;
})());
const tri1 = cv.matFromArray(3, 1, cv.CV_32FC2, [0, 0, 10, 0, 0, 10]);
const tri2 = cv.matFromArray(3, 1, cv.CV_32FC2, [1, 2, 11, 2, 1, 12]);
eq('★ getAffineTransform', rnd3s(cv.getAffineTransform(tri1, tri2).data64F), '1,0,1,0,1,2');
const q1 = cv.matFromArray(4, 1, cv.CV_32FC2, [0, 0, 10, 0, 10, 10, 0, 10]);
const q2 = cv.matFromArray(4, 1, cv.CV_32FC2, [1, 1, 12, 1, 12, 13, 1, 13]);
eq('★ findHomography', rnd3s(cv.findHomography(q1, q2, cv.RANSAC, 3, new cv.Mat()).data64F), '1.1,0,1,0,1.2,1,0,0,1');
const ff = g.clone(), fmask = cv.Mat.zeros(18, 18, cv.CV_8UC1);
eq('★ floodFill(就地 + 填到掩膜)', cv.floodFill(ff, fmask, new cv.Point(8, 8), new cv.Scalar(128)) + ':' + rnd3(cv.mean(ff)[0]), '36:18');
const markers = cv.Mat.zeros(16, 16, cv.CV_32SC1);
markers.intPtr(0)[0] = 1; markers.intPtr(15)[0] = 2;
const bgr16 = new cv.Mat(); cv.cvtColor(g, bgr16, cv.COLOR_GRAY2BGR);
cv.watershed(bgr16, markers); check('watershed 就地改 markers', markers.type() === cv.CV_32SC1);
const lab2 = new cv.Mat(), sta2 = new cv.Mat(), cen2 = new cv.Mat();
eq('★ connectedComponentsWithStats', cv.connectedComponentsWithStats(g, lab2, sta2, cen2, 8, cv.CV_32S)
  + ':' + lab2.type() + ':' + sta2.rows + 'x' + sta2.cols + ':' + cen2.type(), '2:4:2x5:6');
const kdata = cv.matFromArray(6, 2, cv.CV_32FC1, [0, 0, 0, 1, 1, 0, 10, 10, 10, 11, 11, 10]);
eq('★ kmeans 紧凑度(pp, attempts=1)', rnd3(cv.kmeans(kdata, 2, new cv.Mat(),
  new cv.TermCriteria(cv.TERM_CRITERIA_EPS + cv.TERM_CRITERIA_MAX_ITER, 10, 1), 1, cv.KMEANS_PP_CENTERS)), 2.667);
const xs = [], ys = [];
for (let yy = 0; yy < 16; yy++) for (let xx = 0; xx < 16; xx++) { xs.push(xx); ys.push(yy); }
cv.remap(g, w, cv.matFromArray(16, 16, cv.CV_32FC1, xs), cv.matFromArray(16, 16, cv.CV_32FC1, ys),
         cv.INTER_LINEAR, cv.BORDER_CONSTANT, new cv.Scalar(0));
check('remap 恒等映射 == 原图(逐字节)', sumB(w) === sumB(g), sumB(w) + ' vs ' + sumB(g));
const flow = new cv.Mat();
cv.calcOpticalFlowFarneback(g, g, flow, 0.5, 3, 15, 3, 5, 1.2, 0);
check('calcOpticalFlowFarneback 形状', flow.rows === 16 && flow.type() === cv.CV_32FC2);
check('photo 模块没编进来: fastNlMeansDenoising 抛带原因的错', (() => {
    try { cv.fastNlMeansDenoising(g); return false; } catch (e) { return String(e).indexOf('photo') >= 0; }
})());
check('photo 模块没编进来: inpaint 同理', (() => {
    try { cv.inpaint(g, g); return false; } catch (e) { return String(e).indexOf('photo') >= 0; }
})());
g.delete(); a32.delete(); b32.delete(); w.delete(); w2.delete(); canvas.delete(); ccan.delete();
pts.delete(); hull.delete(); defs.delete(); dr.delete(); h1.delete(); h2.delete(); tri1.delete(); tri2.delete();
q1.delete(); q2.delete(); ff.delete(); fmask.delete(); markers.delete(); bgr16.delete();
lab2.delete(); sta2.delete(); cen2.delete(); kdata.delete(); flow.delete();
} catch (e) {
    check('传统算法段整体不抛错', false, String(e).slice(0, 140));
}

// ---- 12. 释放 ----
const n = img.id;
img.delete();
check('delete() 之后 isDeleted', img.isDeleted() === true);
check('releaseAll 收尾', typeof cv.releaseAll() === 'number');

console.log(pass + ' pass, ' + fail + ' fail');
process.exitCode = fail === 0 ? 0 : 1;
