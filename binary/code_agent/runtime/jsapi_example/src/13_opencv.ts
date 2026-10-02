// 13_opencv —— OpenCV.js 形状的 'cv' 包（颜色/滤波/阈值/形态学/轮廓/模板匹配）。
//
// 本文件同时是一份**性能基准**: 同一条流水线
//   · 这里跑 **原生 OpenCV 4.10**（我们 C++ 侧 JZJSCvModule, JS 只做编排）;
//   · node/opencv_bench.mjs 跑 **opencv-js 5.0 / WASM**（npm 上最流行的那个封装）。
// 两边的输入图、步骤、计时口径完全对齐; **API 形状也对齐了** —— findContours 是出参式
// （结果填进 MatVector/hierarchy, 无返回）, 轮廓是"在位"的 Mat、派生量按需问, 凸包/近似多边形
// 也是出参式, threshold 回实际阈值, meanStdDev 收出参…… **所有出参形式一律无返回**
// （`cv.cvtColor(src, dst, code)` 就是 undefined, 只有 threshold->阈值 / connectedComponents->个数
// 两个例外）, Mat 的 `type()/depth()/empty()/elemSize()/step1()` 也都是**方法**。
// 所以下面这段几乎可以逐行搬去 Node 跑。
//
// 跑法:
//   ctest -R jsapi_example.13_opencv          (或)
//   code_runtime --entry <本目录>/build/13_opencv.js
//   Node 对照: node node/opencv_bench.mjs     （先 npm i @techstark/opencv-js, 见那个文件头）
//
// ⚠ 断言只覆盖**结果对不对**（形状/通道/阈值/轮廓数/匹配位置）; 耗时只打印不断言 ——
//   耗时随机器与负载变, 断言进去 ctest 会随机变红。
// ⚠ 计时源: 本引擎没有 performance.now() / process.hrtime（只有 Date.now(), 1ms 粒度）,
//   所以每个步骤**重复 REPS 次取平均**, 1ms 以下的步骤也量得出来。
import cv from 'cv';
import { check, eq } from './helper/assert';

// ————————————————————————————————————————————————————————————
// 实测（2026-10-02, 1280×720 8UC3 合成图, 每步跑到 60ms 取平均; 本机 Release 版 code_runtime）:
//
//   步骤                       JZ(原生 4.10)   JZ(Debug)    Node(opencv-js 5.0/WASM)
//   cvtColor -> gray              0.18           9.6             1.16
//   GaussianBlur 5x5              0.42          20.3             3.92
//   threshold (OTSU)              0.86           2.5             1.44
//   morphologyEx open             1.20          49.5            15.69
//   findContours + 逐条量         6.20          60.5             2.76
//   matchTemplate 64x64          24.0           107              56.5
//   合计                         32.9          ~249             81.5
//
//   ⇒ **纯像素**的 op(滤波/形态学/模板匹配) JZ 快 1.6~13x（原生 vs wasm, 见文件头"不是实现差异"）。
//   ⇒ findContours 一开始慢 10x(27ms): 老写法把每条轮廓的 points/hull/approx **一次性 JSON 化**
//     （65 条轮廓 69KB 文本）。改成 **OpenCV.js 那套（轮廓=Mat 句柄 + 派生量按需问）** 后 → 6.2ms。
//     剩下的 3.4ms 差是"每调一次 op 都要过一遍 JSON 通道"（66 条 × area+bbox = 132 次调用）。
//   ⇒ 量性能必须用 **Release**: Debug 版连 OpenCV 都是 debug DLL, 慢 ~7x, 拿它下结论会错。
// ————————————————————————————————————————————————————————————

// 这个 exe 没编 OpenCV 时优雅跳过: 其它功能都不受影响, 不该让 ctest 变红。
if (!cv.available()) {
    console.log('[SKIP] 本 exe 未编入 OpenCV（JZ_CU_OPENCV=0）—— 13_opencv 跳过');
}
else {
    const W = 1280, H = 720;    // 一屏的分辨率级（1080p 的 2/3, 容器里跑得动）
    // 每个步骤的重复次数**不固定**: 跑到 60ms 为止（见下面的 bench）

    // ————————————————————————————————————————————————————————————
    // 1. 输入: 一张"像屏幕"的合成图
    //    构造不计时; 同一个 PRNG（=> 与 node 对照脚本产出同一张图）。
    // ————————————————————————————————————————————————————————————
    let seed = 20261002;
    function rnd(n: number): number {
        seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff;
        return seed % n;
    }
    function color() {
        return new cv.Scalar(rnd(256), rnd(256), rnd(256));
    }

    const img = new cv.Mat(H, W, cv.CV_8UC3, new cv.Scalar(40, 40, 40));   // 深灰底
    for (let i = 0; i < 60; i++)                                            // 60 个圆（"按钮/小球"）
        cv.circle(img, new cv.Point(rnd(W), rnd(H)), 8 + rnd(24), color(), -1);
    for (let i = 0; i < 30; i++) {                                          // 30 个方块（"面板/图标"）
        const x = rnd(W - 200), y = rnd(H - 120);
        cv.rectangle(img, new cv.Point(x, y), new cv.Point(x + 40 + rnd(160), y + 30 + rnd(90)), color(), -1);
    }
    for (let i = 0; i < 20; i++)                                            // 20 条线（"文字行/边框"）
        cv.line(img, new cv.Point(rnd(W), rnd(H)), new cv.Point(rnd(W), rnd(H)), color(), 2);

    eq('输入图 rows/cols/通道', [img.rows, img.cols, img.channels()], [H, W, 3]);

    /** 跑到**至少 MIN_MS 毫秒**为止, 按实际次数取平均。每轮自己建/释放输出 ——
     *  分配与回收也算进去（两边口径一致）。
     *  ⚠ 不能固定次数: Date.now() 只有 1ms 粒度, Release 下 cvtColor 这类一步不到 1ms,
     *  固定 10 次会算出 0.00 —— 跑够时长才量得准。 */
    const MIN_MS = 60;
    function bench(name: string, once: () => void): number {
        once();                                     // 预热一次（首调带一次性开销）
        const t0 = Date.now();
        let n = 0, el = 0;
        while (n < 4000 && (el = Date.now() - t0) < MIN_MS) {
            once();
            n++;
        }
        const per = el / (n > 0 ? n : 1);
        console.log('  ' + name.padEnd(24) + per.toFixed(2) + ' ms/次  (n=' + n + ')');
        return per;
    }

    console.log('== 流水线耗时（原生 OpenCV 4.10 / JZ 引擎, 每步跑到 ' + MIN_MS + 'ms 取平均） ==');

    // ————————————————————————————————————————————————————————————
    // 2. 逐步骤计时
    //    ⚠ 每个步骤都写成 `dst 出参` 的形状（OpenCV.js 的习惯）: cvtColor(src, dst, code)
    // ————————————————————————————————————————————————————————————
    const tColor = bench('cvtColor -> gray', () => {
        const g = new cv.Mat();
        cv.cvtColor(img, g, cv.COLOR_BGR2GRAY);
        g.delete();
    });

    // 后续步骤的输入（只建一次, 不计时）
    const gray = new cv.Mat();
    cv.cvtColor(img, gray, cv.COLOR_BGR2GRAY);
    eq('灰度图通道数', gray.channels(), 1);

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
        otsu = cv.threshold(blur, t, 0, 255, cv.THRESH_BINARY + cv.THRESH_OTSU);   // 常量相加 = OpenCV 的组合写法
        t.delete();
    });
    check('OTSU 回的是实际阈值(number)', typeof otsu === 'number' && otsu > 0);

    const bin = new cv.Mat();
    cv.threshold(blur, bin, 0, 255, cv.THRESH_BINARY + cv.THRESH_OTSU);

    const kernel = cv.getStructuringElement(cv.MORPH_ELLIPSE, new cv.Size(5, 5));   // shape 也收 MORPH_* 数字
    const tMorph = bench('morphologyEx open', () => {
        const o = new cv.Mat();
        cv.morphologyEx(bin, o, cv.MORPH_OPEN, kernel);
        o.delete();
    });

    const clean = new cv.Mat();
    cv.morphologyEx(bin, clean, cv.MORPH_OPEN, kernel);

    // 轮廓: OpenCV.js 的 `findContours(img, contours, hierarchy, mode, method)` + MatVector
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
            c.delete();                             // 与 Node 一样: 轮廓是句柄, 这里真释放
        }
        hierarchy.delete();
        contours.delete();
    });
    check('找得到轮廓', found > 5);
    check('其中"够大"的有若干个', big > 0);

    // 模板匹配（UI 找图那条路的代表动作）: 拿灰度图里 64x64 的一块当模板, 再找回原位置
    const templ = gray.roi(new cv.Rect(300, 200, 64, 64));
    const tMatch = bench('matchTemplate 64x64', () => {
        const res = new cv.Mat();
        cv.matchTemplate(gray, templ, res, cv.TM_CCOEFF_NORMED);     // OpenCV.js 的顺序: (img, templ, result, method)
        res.delete();
    });

    const res = new cv.Mat();
    cv.matchTemplate(gray, templ, res, cv.TM_CCOEFF_NORMED);         // 出参式: 结果在 res 里, 无返回
    const loc = cv.minMaxLoc(res);
    check('匹配回模板来源位置', Math.abs(loc.maxLoc.x - 300) <= 1 && Math.abs(loc.maxLoc.y - 200) <= 1);
    check('自匹配得分接近 1', loc.maxVal > 0.99);

    // ————————————————————————————————————————————————————————————
    // 3. 结果 + 汇总
    // ————————————————————————————————————————————————————————————
    const total = tColor + tBlur + tThresh + tMorph + tContours + tMatch;
    console.log('  合计 ' + total.toFixed(2) + ' ms/轮');
    console.log('  轮廓 ' + found + ' 条, 其中大块 ' + big + ' 条, OTSU 阈值 ' + otsu);

    // 轮廓的用法（与 node 对照脚本逐行同形）: 出参式 findContours + 按需问量 + 出参式凸包/近似
    const mv2 = new cv.MatVector();
    const hier2 = new cv.Mat();
    cv.findContours(clean, mv2, hier2, cv.RETR_EXTERNAL, cv.CHAIN_APPROX_SIMPLE);
    const top: number[] = [];
    for (let i = 0; i < mv2.size() && i < 3; i++)
        top.push(Math.round(cv.contourArea(mv2.get(i))));
    console.log('  前 3 大面积(按需问, 不搬点): ' + JSON.stringify(top));
    check('轮廓是位在 C++ 的 Mat(句柄)', mv2.get(0) instanceof cv.Mat && top[0] > 0);

    const hull = new cv.Mat();
    cv.convexHull(mv2.get(0), hull);                       // 出参式: 与 cv.convexHull(cnt, hull) 一致
    const ap = new cv.Mat();
    cv.approxPolyDP(mv2.get(0), ap, 2.0, true);            // eps 是绝对阈值
    check('凸包/近似多边形出参形式', hull.rows > 0 && ap.rows > 0 && ap.rows <= mv2.get(0).rows);

    // 本包的扩展: 只给 opts 时一次回"富形态"（每条轮廓的 points/area/hull/approx 都现成, 便于一次性筛）
    const rich = cv.findContours(clean, { mode: cv.RETR_EXTERNAL, method: cv.CHAIN_APPROX_SIMPLE });
    check('扩展: 富形态也能拿', rich.length === mv2.size() && rich[0].area > 0);
    console.log('  (扩展的富形态一次回全量: 这图 ' + rich.length + ' 条轮廓 = '
        + Math.round(JSON.stringify(rich).length / 1024) + ' KB JSON —— 按需问量就没这笔开销)');

    for (let i = 0; i < mv2.size(); i++)
        mv2.get(i).delete();
    mv2.delete(); hier2.delete(); hull.delete(); ap.delete();

    // ————————————————————————————————————————————————————————————
    // 4. 收尾: 句柄都在 C++ 侧, 不主动 free 会一直留着
    // ————————————————————————————————————————————————————————————
    const left = cv.releaseAll();
    check('releaseAll 之后句柄归零', left >= 0 && cv.count() === 0);
}
