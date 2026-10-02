// opencv_bench.mjs —— 13_opencv.ts 的 **Node 对照脚本**：同一条流水线, 跑在
// opencv-js（@techstark/opencv-js, npm 上最流行的那个封装, 5.0.0-release.1 = OpenCV 5.x 的 WASM 版)上。
//
// 为什么要它: 想量"我们自己那套(cv 包 + 原生 OpenCV 4.10, JS 只做编排)"到底快多少/慢多少,
// 得有同一把尺子。本文件与 13_opencv.ts **逐步骤对齐**: 同一个 PRNG(=> 同一张输入图)、
// 同分辨率、同重复次数、同"每轮自建自释放"的口径。
//
// 跑法:
//   npm i @techstark/opencv-js
//   node opencv_bench.mjs
// 实测环境(2026-10-02): node v22.23.2(仓库自带的那份), @techstark/opencv-js 5.0.0-release.1。
// 结果(同机同图, 1280×720; 详细对照表见 13_opencv.ts 的文件头):
//   cvtColor 1.16 / GaussianBlur 3.92 / threshold 1.44 / morphologyEx 15.69 /
//   findContours 2.76 / matchTemplate 56.5  ⇒ 合计 81.5 ms/轮
//   JZ(原生 4.10, Release): 0.18 / 0.42 / 0.86 / 1.20 / 6.2 / 24.0 ⇒ 合计 32.9 ms/轮
//   (纯像素的 op 我们快 1.6~13x; findContours 现在只差 3.4ms —— 那是我们"每次 op 都要过一遍
//    JSON 通道"的开销, 见 13_opencv.ts 的文件头)
//
// 口径差异（读结果时要知道）:
//   · JZ 侧图片在 C++ 里（JS 只拿句柄, 零拷贝）; 这边每次 new cv.Mat 都在 WASM 堆上分配。
//   · opencv-js 的 CV_* 用 **OpenCV 5.x 的通道编码**（CV_8UC3 = 64, JZ 那边是 4.x = 16),
//     所以两边都写 `cv.CV_8UC3` 而不是数字。
//   · JZ 侧 findContours 返回的 Contour **自带** area/boundingRect/hull/approx（一次算完）;
//     这边必须逐条调 cv.contourArea/cv.boundingRect（本例两边都走"逐条调"的写法, 保持可比）。
//   · 计时源: 这边用 process.hrtime.bigint()（纳秒）; JZ 引擎没有它, 那边用 Date.now() + 多次平均。

import mod from '@techstark/opencv-js';

let cv = mod;
if (cv && typeof cv.then === 'function')      // 这个包在 ESM 下 default 是 thenable（等它 resolve）
    cv = await cv;
if (cv && cv.default)
    cv = cv.default;
if (!cv.Mat)                                  // 还没初始化完: 等运行时回调
    await new Promise(function (resolve) { cv.onRuntimeInitialized = function () { resolve(); }; });

const ns = function () { return Number(process.hrtime.bigint()) / 1e6; };   // 毫秒(小数)

const W = 1280, H = 720;
const MIN_MS = 60;   // 与 JZ 侧同一个口径: 每步跑到至少 60ms

// ————————————————————————————————————————————————————————————
// 1. 输入图（与 13_opencv.ts 同一个 PRNG => 同一张图）
// ————————————————————————————————————————————————————————————
let seed = 20261002;
function rnd(n) {
    seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
    return seed % n;
}
function color() {
    return new cv.Scalar(rnd(256), rnd(256), rnd(256));
}

const img = new cv.Mat(H, W, cv.CV_8UC3, new cv.Scalar(40, 40, 40));
for (let i = 0; i < 60; i++)
    cv.circle(img, new cv.Point(rnd(W), rnd(H)), 8 + rnd(24), color(), -1);
for (let i = 0; i < 30; i++) {
    const x = rnd(W - 200), y = rnd(H - 120);
    cv.rectangle(img, new cv.Point(x, y), new cv.Point(x + 40 + rnd(160), y + 30 + rnd(90)), color(), -1);
}
for (let i = 0; i < 20; i++)
    cv.line(img, new cv.Point(rnd(W), rnd(H)), new cv.Point(rnd(W), rnd(H)), color(), 2);
console.log('输入图 = ' + img.rows + 'x' + img.cols + 'x' + img.channels());

function bench(name, once) {
    once();                                 // 预热
    const t0 = ns();
    let n = 0, el = 0;
    // 与 JZ 侧同一个口径: 跑到至少 60ms 为止, 按实际次数平均（那边只有 1ms 粒度, 固定次数会算出 0.00）
    while (n < 4000 && (el = ns() - t0) < 60) {
        once();
        n++;
    }
    const per = el / (n > 0 ? n : 1);
    console.log('  ' + name.padEnd(24) + per.toFixed(2) + ' ms/次  (n=' + n + ')');
    return per;
}

console.log('== 流水线耗时（opencv-js 5.0 / WASM, 每步跑到 ' + MIN_MS + 'ms 取平均） ==');

// ————————————————————————————————————————————————————————————
// 2. 逐步骤计时（与 13_opencv.ts 一一对应）
// ————————————————————————————————————————————————————————————
const tColor = bench('cvtColor -> gray', () => {
    const g = new cv.Mat();
    cv.cvtColor(img, g, cv.COLOR_BGR2GRAY);
    g.delete();
});

const gray = new cv.Mat();
cv.cvtColor(img, gray, cv.COLOR_BGR2GRAY);

const ksize = new cv.Size(5, 5);
const tBlur = bench('GaussianBlur 5x5', () => {
    const b = new cv.Mat();
    cv.GaussianBlur(gray, b, ksize, 0);
    b.delete();
});

const blur = new cv.Mat();
cv.GaussianBlur(gray, blur, ksize, 0);

let otsu = 0;
const tThresh = bench('threshold (OTSU)', () => {
    const t = new cv.Mat();
    otsu = cv.threshold(blur, t, 0, 255, cv.THRESH_BINARY + cv.THRESH_OTSU);
    t.delete();
});

const bin = new cv.Mat();
cv.threshold(blur, bin, 0, 255, cv.THRESH_BINARY + cv.THRESH_OTSU);

const kernel = cv.getStructuringElement(cv.MORPH_ELLIPSE, new cv.Size(5, 5));
const tMorph = bench('morphologyEx open', () => {
    const o = new cv.Mat();
    cv.morphologyEx(bin, o, cv.MORPH_OPEN, kernel);
    o.delete();
});

const clean = new cv.Mat();
cv.morphologyEx(bin, clean, cv.MORPH_OPEN, kernel);

let found = 0, big = 0;
const tContours = bench('findContours + 逐条量', () => {
    const contours = new cv.MatVector();
    const hierarchy = new cv.Mat();
    cv.findContours(clean, contours, hierarchy, cv.RETR_EXTERNAL, cv.CHAIN_APPROX_SIMPLE);
    found = contours.size();
    big = 0;
    for (let i = 0; i < contours.size(); i++) {
        const c = contours.get(i);
        if (cv.contourArea(c) > 200 && cv.boundingRect(c).width > 12)
            big++;
        c.delete();
    }
    hierarchy.delete();
    contours.delete();
});
console.log('  轮廓 ' + found + ' 条, 其中大块 ' + big + ' 条, OTSU 阈值 ' + otsu);

const templ = gray.roi(new cv.Rect(300, 200, 64, 64));
const tMatch = bench('matchTemplate 64x64', () => {
    const res = new cv.Mat();
    cv.matchTemplate(gray, templ, res, cv.TM_CCOEFF_NORMED);
    res.delete();
});

const res = new cv.Mat();
cv.matchTemplate(gray, templ, res, cv.TM_CCOEFF_NORMED);
const loc = cv.minMaxLoc(res);
console.log('  匹配位置 = (' + loc.maxLoc.x + ',' + loc.maxLoc.y + ') 得分 = ' + loc.maxVal.toFixed(4));

// ————————————————————————————————————————————————————————————
// 3. 汇总
// ————————————————————————————————————————————————————————————
const total = tColor + tBlur + tThresh + tMorph + tContours + tMatch;
console.log('  合计 ' + total.toFixed(2) + ' ms/轮');
console.log('  (opencv-js 没有 releaseAll/count: 手工 delete 上面的图)');
for (const m of [img, gray, blur, bin, clean, kernel, templ, res]) {
    if (m && m.delete) m.delete();
}
