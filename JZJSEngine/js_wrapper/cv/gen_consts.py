# -*- coding: utf-8 -*-
"""由 `consts.json`(从真 opencv-js 导出的常量快照) 生成 TS 常量表 cv/consts.ts。

重新导出: 在装了 @techstark/opencv-js 的目录里跑
    node -e "const cv=require('@techstark/opencv-js'); ..."   # 见 cv/README 里的 dump_const 用法
本仓库保留 consts.json 是为了让这张表可复现(生成 -> 与 OpenCV 4.10 头文件核对 -> 写回 consts.ts)。
"""
import json
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "consts.json")
if not os.path.exists(RAW):
    RAW = os.path.expandvars(r"%TEMP%\jz_cv_ref\consts.json")
OUT = os.path.join(HERE, "consts.ts")

data = json.load(open(RAW, encoding="utf8"))

# 只保留与本封装有关的族; 丢掉 INTER_BITS / INTER_MAX / NORM_TYPE_MASK 之类"实现细节常数"
DROP = {"INTER_BITS", "INTER_BITS2", "INTER_MAX", "INTER_TAB_SIZE", "INTER_TAB_SIZE2", "NORM_TYPE_MASK",
        "HISTCMP_EMD"}          # opencv-js 导出了这个名字, 但 OpenCV 4.10 的 cv:: 里没有(见 5 = KL_DIV)
ORDER = ["CV_", "COLOR_", "THRESH_", "MORPH_", "RETR_", "CHAIN_APPROX_", "INTER_", "BORDER_",
         "TM_", "NORM_", "CMP_", "IMREAD_", "FONT_HERSHEY_", "LINE_", "COLORMAP_",
         # 2026-10-03 补: 传统算法那批用到的码表
         "DIST_", "HOUGH_", "KMEANS_", "TERM_CRITERIA_", "HISTCMP_", "CALIB_", "WARP_", "INPAINT_",
         "RANSAC_", "MISC_"]

# ⚠ CV_* 的类型常量**不能**照抄 opencv-js 5.x: OpenCV 5 把通道位移改成 5 位(8UC3 = 64),
#   我们链接的是 4.x(CN_SHIFT = 3, 8UC3 = 16), 照抄会让 cv::Mat 的类型算成 9 通道。
#   这里按 4.x 的 CV_MAKETYPE(depth, cn) = depth + ((cn-1) << 3) 生成。
DEPTHS = {"8U": 0, "8S": 1, "16U": 2, "16S": 3, "32S": 4, "32F": 5, "64F": 6}
cv_lines = ["// ---- CV_ (OpenCV 4.x 的通道编码: CV_MAKETYPE(depth,cn) = depth + ((cn-1) << 3)) ----"]
for d, base in DEPTHS.items():
    cv_lines.append("export const CV_%s = %d;" % (d, base))
for d, base in DEPTHS.items():
    for cn in range(1, 5):
        cv_lines.append("export const CV_%sC%d = %d;" % (d, cn, base + ((cn - 1) << 3)))
cv_lines.append("export const CV_16F = 7;")
cv_lines.append("")

# opencv-js 的导出里没有 IMREAD_*/ADAPTIVE_THRESH_*/DECOMP_*(它们是 imread 的 flag /
# adaptiveThreshold 的 method / Mat.inv 的 method), 但 OpenCV.js 教程里常写;
# 取值同样是 OpenCV 4.x 的稳定值(与 opencv-js 同值)。
extra = """// ---- IMREAD_ (imread 的 flag; OpenCV 4.x 稳定值, opencv-js 的导出里没有这些名字) ----
export const IMREAD_UNCHANGED = -1;
export const IMREAD_GRAYSCALE = 0;
export const IMREAD_COLOR = 1;
export const IMREAD_ANYDEPTH = 2;
export const IMREAD_ANYCOLOR = 4;
export const IMREAD_IGNORE_ORIENTATION = 128;

// ---- ADAPTIVE_THRESH_ (adaptiveThreshold 的 method) ----
export const ADAPTIVE_THRESH_MEAN_C = 0;
export const ADAPTIVE_THRESH_GAUSSIAN_C = 1;

// ---- DECOMP_ (Mat.inv / cv::invert 的 method) ----
export const DECOMP_LU = 0;
export const DECOMP_SVD = 1;
export const DECOMP_EIG = 2;
export const DECOMP_CHOLESKY = 3;
export const DECOMP_QR = 4;
export const DECOMP_NORMAL = 16;

// ---- TERM_CRITERIA_ (迭代算法终止条件; opencv-js 的导出里没有, 但教程/KMEANS 里常写) ----
export const TERM_CRITERIA_COUNT = 1;
export const TERM_CRITERIA_MAX_ITER = 1;
export const TERM_CRITERIA_EPS = 2;
"""

rows = list(cv_lines)
total = len([l for l in cv_lines if l.startswith("export const")])
emitted = set()                  # 全局去重: 同一个名字可能在多个族里出现(MISC_ 与 INTER_ 等)
for g in ORDER:
    if g == "CV_":
        continue                 # 上面按 4.x 公式生成过
    items = []
    for it in data.get(g, []):
        name, val = it.rsplit("=", 1)
        items.append((name, int(val)))
    items.sort()
    if not items:
        continue
    rows.append("// ---- %s ----" % g.rstrip("_").replace("CHAIN_APPROX", "CHAIN_APPROX"))
    for name, val in items:
        if name in emitted or name in DROP:
            continue
        emitted.add(name)
        rows.append("export const %s = %d;" % (name, val))
        total += 1
    rows.append("")

header = '''// cv 常量表 —— OpenCV.js 的 cv.XXX 在这里**同名同值**。
//
// 数值不是手抄的: 2026-10-02 从真 opencv-js(@techstark/opencv-js 5.0.0-release.1)导出, 再用本机
// OpenCV **4.10 头文件**逐个核对过(274 个名字零差异), 所以跟 OpenCV.js 教程/示例里的
// cv.COLOR_BGR2GRAY / cv.THRESH_BINARY + cv.THRESH_OTSU 完全是同一个数。
//
// ⚠ 唯一的例外是 CV_* 一族: OpenCV 5 把通道位数从 3 位改成 5 位(8UC3 = 64), 而我们是 4.x
//   (8UC3 = **16**), 照抄 5.x 会让 cv::Mat 算成 9 通道。这一族按 4.x 的
//   CV_MAKETYPE(depth,cn) = depth + ((cn-1) << 3) 生成。
//
// 为什么可以只给数字: C++ 侧码表参数(colorCodeOf / threshOf / morphOf / retrOf / chainOf /
// interpOf / borderOf / tmOf / normOf / cmpOf / fontOf / colormapOf)都加了"整型直通"
// (JZJSCvModule.cpp 的 numericKey), 数字原样交给 OpenCV —— 于是**官方有而我们名字表里没有**
// 的那些码(如 COLOR_BGR2Luv / BORDER_TRANSPARENT / TM_SQDIFF)也照样能用。
//
// ⚠ 本文件由 gen_consts.py + consts.json 生成; 改值请以 OpenCV 官方 header 为准。
/* eslint-disable */

'''

src = header + "\n".join(rows).rstrip() + "\n\n" + extra
open(OUT, "w", encoding="utf8", newline="\n").write(src)
print("wrote %s, %d constants, %d lines" % (OUT, total, src.count("\n") + 1))
