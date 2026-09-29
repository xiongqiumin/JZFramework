#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图标查找回归测试(先用 OpenCV 后端, VLM 后端预留接口)。

被测的能力(也就是"给一张截图 + 图标模板集, 产出一个模型, 用它找到屏幕上的图标"):
    build(图标目录) -> <model>/model.json + templates.npz      # 产出"一个模型"
    find (截图, 模型) -> [{label, score, rect, nrect, center}]  # 用模型查图标

断言(全部是"硬判定": 数量 + 类别 + 像素级位置, 没有模糊地带):
    1. build 真的产出了模型文件, 且能重新加载(version/items/labels 对得上)
    2. 四张合成截图(浅色/深色主题 × 两个 seed)上:
       召回 = GT 数量(含"同类图标同屏出现两次"的实例) / 类别必须正确 / 零误检 /
       每个命中框的中心点与 GT 中心误差 <= 容差
    3. region 裁剪查找: 结果坐标仍以**整屏**为基准, 且只返回区域内的图标
    4. `--backend vlm` 明确报"未实现"(退出码 3) —— 不允许静默回退到别的后端,
       这条断言就是"后续接 VLM/YOLO 后端"的接口契约
    5. 每张图的耗时打印出来(CPU-only 目标机要看的就是这个数)

为什么用合成截图: 目标是回归"查找机制"(多实例、缩放、深浅主题、零误检),
    所以 GT 必须精确到像素 ⇒ 在代码里画场景(图标用矢量图元), 缺 numpy/opencv 时返回 77 = 跳过。

实测(本机 CPU, 无 GPU): 6 张合成截图 42/42 命中、0 误检、中心点误差 <= 1px、单帧 1~4 秒。
    最难的样本是 22px 的细圆环(综合分 0.58, 而误检天花板 0.54) —— 余量很薄, 这是模板匹配的固有边界,
    也正是"要上 YOLO/VLM 后端"的实证理由; 这里把它固化成回归, 换后端时同一批断言继续用。

用法(也是 ctest 条目 agent_cli.icon_find 的实现):
    ctest -C Release -R agent_cli.icon_find --output-on-failure
    python test_icon_find.py [--score 0.55] [--keep] [--tol 4.0]
"""
from __future__ import annotations

import argparse
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import icon_finder as IF
except SystemExit as exc:                     # 缺 numpy/opencv 时 icon_finder 直接 77 退出
    print("[icon_find] skip: dependency missing (%s)" % exc.code)
    raise SystemExit(77)

FAILED = []


def log(msg: str = "") -> None:
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "utf-8"
        print(msg.encode(enc, "replace").decode(enc, "replace"), flush=True)


def check(cond: bool, what: str) -> bool:
    if cond:
        log("  [ok]   %s" % what)
    else:
        log("  [FAIL] %s" % what)
        FAILED.append(what)
    return cond


def match_gt_hits(gt_items, hits, tol_px: float):
    """按中心点贪心配对 ⇒ (配对, 漏检的 GT, 多出的命中)。配对还要求 label 一致。"""
    used, pairs, missed = set(), [], []
    for g in gt_items:
        gx, gy = g["rect"]["x"] + g["rect"]["w"] / 2.0, g["rect"]["y"] + g["rect"]["h"] / 2.0
        best, best_d = None, None
        for i, h in enumerate(hits):
            if i in used:
                continue
            d = math.hypot(h["center"]["x"] - gx, h["center"]["y"] - gy)
            if best is None or d < best_d:
                best, best_d = i, d
        if best is not None and best_d <= tol_px and hits[best]["label"] == g["label"]:
            pairs.append((g, hits[best], best_d))
            used.add(best)
        else:
            missed.append(g)
    extra = [h for i, h in enumerate(hits) if i not in used]
    return pairs, missed, extra


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--score", type=float, default=IF.DEFAULT_SCORE)
    ap.add_argument("--tol", type=float, default=4.0, help="中心点容差(像素)")
    ap.add_argument("--keep", action="store_true", help="保留临时目录(排查用)")
    args = ap.parse_args()

    work = tempfile.mkdtemp(prefix="jz_icon_find_")
    icons_dir = os.path.join(work, "icons")
    model_dir = os.path.join(work, "model")
    scene_dir = os.path.join(work, "scenes")
    try:
        # ---- 1. 素材 + 模型 ----
        log("[1] 合成图标模板 -> 模型")
        IF.generate_icons(icons_dir)
        t0 = time.time()
        model = IF.build_model(icons_dir, model_dir, score=args.score)
        build_ms = (time.time() - t0) * 1000.0
        check(os.path.isfile(IF.model_path(model_dir)), "model.json 已产出")
        check(os.path.isfile(os.path.join(model_dir, "templates.npz")), "templates.npz 已产出")
        reloaded = IF.load_model(model_dir)
        want_items = len(IF.ICON_NAMES) * len(model["sizes"])
        check(reloaded["items"] and len(reloaded["items"]) == want_items,
              "模型可重新加载且模板项数 = %d" % want_items)
        check(reloaded["labels"] == sorted(IF.ICON_NAMES),
              "模型类别 = %s" % ",".join(reloaded["labels"]))
        log("      build %d 项模板, 用时 %.0f ms" % (len(model["items"]), build_ms))

        # ---- 2. 四张场景: 召回 / 类别 / 误检 / 中心点误差 ----
        log("[2] 合成截图查找(浅色/深色 × seed 1,2)")
        total_gt = total_hits = 0
        worst = 0.0
        slowest = 0.0
        for theme in ("light", "dark"):
            for seed in (1, 2):
                gt = IF.generate_scene(icons_dir, scene_dir, seed=seed, theme=theme)
                t0 = time.time()
                hits = IF.find_icons(model_dir, gt["screen"])
                dt = (time.time() - t0) * 1000.0
                slowest = max(slowest, dt)
                pairs, missed, extra = match_gt_hits(gt["items"], hits, args.tol)
                worst = max([worst] + [d for _g, _h, d in pairs]) if pairs else worst
                total_gt += len(gt["items"])
                total_hits += len(hits)
                log("  -- %s/seed=%d: gt=%d hits=%d t=%.0fms"
                    % (theme, seed, len(gt["items"]), len(hits), dt))
                check(not missed, "%s/seed=%d 无漏检(漏 %d: %s)"
                      % (theme, seed, len(missed), ",".join(g["label"] for g in missed[:3])))
                check(not extra, "%s/seed=%d 无误检(多 %d)" % (theme, seed, len(extra)))
                check(len(pairs) == len(gt["items"]),
                      "%s/seed=%d 命中数量与 GT 一致(%d/%d)"
                      % (theme, seed, len(pairs), len(gt["items"])))
        check(total_gt > 0 and total_hits >= total_gt, "总体召回 %d/%d" % (total_hits, total_gt))
        log("      中心点最大误差 %.2f px(容差 %.1f), 单帧最慢 %.0f ms" % (worst, args.tol, slowest))
        check(slowest < 30000, "单帧查找 < 30s(实测最慢 %.0f ms)" % slowest)

        # ---- 3. region 裁剪: 坐标仍是整屏基准, 只返回区域内的图标 ----
        log("[3] region 裁剪查找")
        gt = IF.generate_scene(icons_dir, scene_dir, seed=3, theme="light")
        w, h = gt["width"], gt["height"]
        region = (w // 2, h // 3, w - w // 2, h - h // 3)      # 右下 2/3
        hits = IF.find_icons(model_dir, gt["screen"], region=region)
        rx, ry, rw, rh = region
        inside = [g for g in gt["items"]
                  if rx <= g["rect"]["x"] + g["rect"]["w"] / 2 <= rx + rw
                  and ry <= g["rect"]["y"] + g["rect"]["h"] / 2 <= ry + rh]
        check(all(rx <= h_["center"]["x"] <= rx + rw and ry <= h_["center"]["y"] <= ry + rh
                  for h_ in hits), "命中都落在 region 内(坐标以整屏为基准, 不是相对 region)")
        pairs, missed, extra = match_gt_hits(inside, hits, args.tol)
        check(not missed, "region 内 GT 全部找到(漏 %d)" % len(missed))
        check(not extra, "region 外/干扰没有被返回(多 %d)" % len(extra))
        log("      region=%s: 区域内 gt=%d hits=%d" % (region, len(inside), len(hits)))

        # ---- 4. 后端接口契约: vlm 未实现必须响亮地失败 ----
        log("[4] --backend vlm 接口契约")
        rc = subprocess.run([sys.executable, os.path.join(HERE, "icon_finder.py"),
                             "build", "--icons", icons_dir, "--out", os.path.join(work, "m_vlm"),
                             "--backend", "vlm"], capture_output=True, text=True)
        check(rc.returncode == 3 and "not implemented" in (rc.stdout + rc.stderr),
              "vlm 后端返回 3 且提示 not implemented(rc=%d)" % rc.returncode)

        # ---- 5. 结果 ----
        log("[5] JSON 契约")
        hits = IF.find_icons(model_dir, gt["screen"], labels=("star",))
        check(all(h["label"] == "star" for h in hits), "labels 过滤只返回 star")
        if hits:
            need = {"label", "score", "rect", "nrect", "center"}
            check(need <= set(hits[0].keys()), "命中字段齐全 %s" % ",".join(sorted(need)))
            check(0.0 <= hits[0]["nrect"]["x"] <= 1.0, "nrect 是归一化坐标")
    finally:
        if args.keep:
            log("[i] 临时目录保留: %s" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)

    if FAILED:
        log("[icon_find] FAIL: %d 项断言未通过" % len(FAILED))
        for f in FAILED:
            log("    - %s" % f)
        return 1
    log("[icon_find] PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
