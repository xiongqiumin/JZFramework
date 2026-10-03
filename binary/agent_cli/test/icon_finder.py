#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图标查找器: 输入截图 + 图标模板集, 先产出一个"图标库模型", 再用它定位屏幕上的图标。

为什么把"产模型"和"查找"分成两步 ——
    build(模板目录) -> <modelDir>/{model.json, templates.npz}   # 只在模板集变动时跑
    find (截图, 模型) -> [{label, score, rect, nrect, center}]   # 每次查找跑
    换后端(YOLO / VLM)只换这两步的实现, 产物格式与调用方式不变。本文件先落 OpenCV 版。

OpenCV 版为什么能同时吃浅色/深色主题 ——
    匹配特征是**边缘幅值**(edge magnitude), 不是灰度: 同一个图标在浅色主题(深色图标)与
    深色主题(白色图标)下灰度是相反的, 但形状/边缘不变。
    多尺度模板(每个图标渲染成一串尺寸) + 峰值聚类(支持同屏多个同类图标) + 全局 NMS
    + 覆盖率闸门(命中处边缘密度必须和模板接近, 用来挡"平背景/细线条"的误命)。

坐标两套都给: rect 是像素(截图坐标系 —— **这才是 CU 的 CuRect 口径**, 2026-10-03 起 CU 对外
也是物理像素), nrect 是归一化 [0,1](沿用给老调用方的兼容字段)。

用法:
    python icon_finder.py build --icons <图标目录> --out <模型目录>
    python icon_finder.py find  --model <模型目录> --screen <截图.png> [--region x,y,w,h] [--json 结果.json]
    python icon_finder.py selftest                 # 合成场景自测(建模型 -> 查找 -> 打印命中)
退出码: 0 正常 / 1 失败 / 2 用法错 / 3 后端未实现 / 77 缺 numpy 或 opencv(ctest 视为跳过)
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time

try:
    import numpy as np
    import cv2
except Exception as _exc:  # noqa: BLE001
    print("[icon_finder] numpy/opencv unavailable: %s" % _exc)
    raise SystemExit(77)  # 77 = ctest SKIP_RETURN_CODE(跳过而不是失败)

MODEL_VERSION = 1
# 模板库尺寸(像素): 小尺寸一像素一档(细线图标对尺寸敏感 —— 步长 2 会把 27px 的圆环配到 28px 上, NCC 掉到 0.59),
# 大尺寸改成 2 像素一档(40px 上差 1px 只有 2.5% 缩放误差, 不值得多花一半推理时间)。
DEFAULT_SIZES = tuple(range(20, 33)) + tuple(range(34, 53, 2))
DEFAULT_SCORE = 0.55                      # 综合分阈值: 综合分 = min(NCC, 倒角验证分), 取两者中差的那个
DEFAULT_NMS_IOU = 0.32
STAGE1_NCC = 0.45                         # 第一段(NCC)负责出候选, 门槛压低; 真伪交由闸门判定
NCC_MIN = 0.50                            # 闸门一(预筛): 形状相关性
VERIFY_MIN = 0.55                         # 闸门二(预筛): 倒角验证 —— 模板的强边要真的落在屏幕强边上
VERIFY_TAU = 0.15                         # 允许的平均错位 = 模板边长的这个比例(细线小图标对错位敏感)
# 阈值怎么来的(6 张合成截图实测): 最难的样本(22px 细圆环)NCC=0.58, 误检天花板 0.54
# ⇒ 综合分门槛取 0.55。余量很薄, 这是模板匹配的固有边界 —— 也正是后面要换 YOLO/VLM 后端的实证理由。
EDGE_BIN = 0.15                           # "这条边算不算数"的阈值
EDGE_BIN_HI = 0.30                        # 倒角验证用的强边阈值
PNG_EXT = (".png", ".jpg", ".jpeg", ".bmp")
ICON_NAMES = ("ring", "star", "arrow", "plus", "folder", "trash")


def log(msg: str = "") -> None:
    # Windows 控制台是本地编码(GBK): 编不出的字符换成 '?' 而不是抛异常
    try:
        print(msg, flush=True)
    except UnicodeEncodeError:
        enc = sys.stdout.encoding or "utf-8"
        print(msg.encode(enc, "replace").decode(enc, "replace"), flush=True)


# ---------------------------------------------------------------------------
# 读写: 路径可能是中文/非 ASCII, cv2.imread / imwrite 在这上面会静默失败
# ---------------------------------------------------------------------------
def imwrite_u(path: str, img) -> None:
    ok, buf = cv2.imencode(os.path.splitext(path)[1] or ".png", img)
    if not ok:
        raise RuntimeError("imencode failed: %s" % path)
    buf.tofile(path)


def imread_u(path: str, flags: int = cv2.IMREAD_UNCHANGED):
    data = np.fromfile(path, dtype=np.uint8)
    if data.size == 0:
        raise RuntimeError("cannot read image: %s" % path)
    img = cv2.imdecode(data, flags)
    if img is None:
        raise RuntimeError("cannot decode image: %s" % path)
    return img


def model_path(model_dir: str) -> str:
    return os.path.join(model_dir, "model.json")


def load_model(model_dir: str) -> dict:
    with open(model_path(model_dir), "r", encoding="utf-8") as f:
        model = json.load(f)
    if int(model.get("version", 0)) != MODEL_VERSION:
        raise RuntimeError("model version mismatch: %s" % model.get("version"))
    return model


# ---------------------------------------------------------------------------
# 特征: 边缘幅值
# ---------------------------------------------------------------------------
def _gray_of(img) -> "np.ndarray":
    if img.ndim == 2:
        return img.astype(np.float32)
    if img.shape[2] == 4:
        return cv2.cvtColor(img[:, :, :3], cv2.COLOR_BGR2GRAY).astype(np.float32)
    return cv2.cvtColor(img, cv2.COLOR_BGR2GRAY).astype(np.float32)


def _icon_gray(img) -> "np.ndarray":
    """图标 -> 灰度。带 alpha 时把透明区填到**与图形相反的一侧**(图形亮就填黑, 暗就填白):
    这样"轮廓"在边缘图里一定是一条强边, 而不是被填成同色后消失
    (踩过: 填成"图形均值"时, 纯色图标(十字/文件夹/圆环)模板会变成一片均匀灰度, 模板边缘全丢, NCC 直接失效)。
    真值背景的明暗不影响判定 —— 边缘幅值只看梯度大小, 不看方向, 所以深浅色主题都能对上。"""
    gray = _gray_of(img)
    if img.ndim == 3 and img.shape[2] == 4:
        m = img[:, :, 3] > 0
        if m.any():
            inside = float(gray[m].mean())
            gray = gray.copy()
            gray[~m] = 0.0 if inside >= 128.0 else 255.0
    return gray


def _edges(gray) -> "np.ndarray":
    g = cv2.GaussianBlur(gray, (3, 3), 0)
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    mag = cv2.magnitude(gx, gy)
    hi = float(np.percentile(mag, 99.0))
    if hi <= 1e-6:
        hi = 1e-6
    return np.clip(mag / hi, 0.0, 1.0).astype(np.float32)


# ---------------------------------------------------------------------------
# build: 模板集 -> 模型
# ---------------------------------------------------------------------------
def build_model(icons_dir: str, out_dir: str, backend: str = "opencv",
                sizes=DEFAULT_SIZES, score: float = DEFAULT_SCORE) -> dict:
    if backend != "opencv":
        raise NotImplementedError("backend '%s' not implemented yet (opencv only)" % backend)
    files = [os.path.join(icons_dir, fn) for fn in sorted(os.listdir(icons_dir))
             if os.path.splitext(fn)[1].lower() in PNG_EXT]
    if not files:
        raise RuntimeError("no icon images in %s" % icons_dir)

    arrays, items = {}, []
    for path in files:
        label = os.path.splitext(os.path.basename(path))[0]
        gray = _icon_gray(imread_u(path))
        for s in sizes:
            t = cv2.resize(gray, (int(s), int(s)), interpolation=cv2.INTER_AREA)
            e = _edges(t)
            key = "e%d" % len(items)
            arrays[key] = e.astype(np.float16)
            items.append({"label": label, "size": int(s), "key": key,
                          "strong": int((e >= EDGE_BIN_HI).sum())})

    os.makedirs(out_dir, exist_ok=True)
    np.savez_compressed(os.path.join(out_dir, "templates.npz"), **arrays)
    model = {
        "version": MODEL_VERSION,
        "backend": "opencv",
        "labels": sorted({it["label"] for it in items}),
        "sizes": [int(s) for s in sizes],
        "score": float(score),
        "stage1_ncc": STAGE1_NCC,
        "ncc_min": NCC_MIN,
        "verify_min": VERIFY_MIN,
        "verify_tau": VERIFY_TAU,
        "nms_iou": DEFAULT_NMS_IOU,
        "templates": "templates.npz",
        "items": items,
        "created": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    with open(model_path(out_dir), "w", encoding="utf-8") as f:
        json.dump(model, f, ensure_ascii=False, indent=1)
    return model


# ---------------------------------------------------------------------------
# find: 截图 + 模型 -> 命中列表
# ---------------------------------------------------------------------------
def _peaks(result, thr: float):
    """阈值图中的每个连通块取一个峰值 —— 同屏出现多个同类图标时这里必须都能出峰。"""
    m = (result >= thr).astype(np.uint8)
    n, _lab, stats, _cent = cv2.connectedComponentsWithStats(m, 8)
    out = []
    for i in range(1, n):
        x, y, w, h, _area = stats[i]
        _minv, maxv, _minl, maxl = cv2.minMaxLoc(result[y:y + h, x:x + w])
        out.append((float(maxv), x + maxl[0], y + maxl[1]))
    return out


def _iou(a, b) -> float:
    ax2, ay2, bx2, by2 = a["x"] + a["w"], a["y"] + a["h"], b["x"] + b["w"], b["y"] + b["h"]
    ix, iy = max(a["x"], b["x"]), max(a["y"], b["y"])
    iw, ih = min(ax2, bx2) - ix, min(ay2, by2) - iy
    if iw <= 0 or ih <= 0:
        return 0.0
    inter = iw * ih
    return inter / float(a["w"] * a["h"] + b["w"] * b["h"] - inter)


def _suppress(a, b, iou_thr: float) -> bool:
    """一个位置只留一个框:
    - 多尺度会把同一个实例配成好几个嵌套框(中心几乎重合, 或小框被大框盖住一大半) ⇒ 这些都要压掉;
    - 但**相邻的不同图标**必须各留一个 ⇒ 判定用"小框被覆盖的比例", 而不是粗暴的 IoU。"""
    ax2, ay2 = a["x"] + a["w"], a["y"] + a["h"]
    bx2, by2 = b["x"] + b["w"], b["y"] + b["h"]
    iw = min(ax2, bx2) - max(a["x"], b["x"])
    ih = min(ay2, by2) - max(a["y"], b["y"])
    if iw > 0 and ih > 0:
        small_area = min(a["w"] * a["h"], b["w"] * b["h"])
        if (iw * ih) / float(small_area) >= 0.55:
            return True
    dist = math.hypot(a["cx"] - b["cx"], a["cy"] - b["cy"])
    small = min(a["w"], a["h"], b["w"], b["h"])
    return dist <= 0.4 * small or _iou(a, b) >= iou_thr


def _nms(cands, iou_thr: float):
    keep = []
    for c in sorted(cands, key=lambda c: -c["score"]):
        if not any(_suppress(c, k, iou_thr) for k in keep):
            keep.append(c)
    return keep


def _verify(dt, tmpl, x: int, y: int, tau: float) -> float:
    """倒角验证: 模板上的每一条强边, 到屏幕最近强边的平均距离有多远。
    NCC 只看"相关性", 平背景/细线条也能糊出高分; 倒角距离是**几何**判定 ⇒ 边缘必须真的对得上。
    这是两段式的第二段, 专治"像但不是"的误命。"""
    ys, xs = np.nonzero(tmpl >= EDGE_BIN_HI)
    if ys.size == 0:
        return 0.0
    d = dt[ys + y, xs + x]
    return float(np.clip(1.0 - float(d.mean()) / max(tau, 1e-6), 0.0, 1.0))


def find_icons(model_dir: str, screen_path: str, region=None, score=None, labels=None):
    model = load_model(model_dir)
    bank = np.load(os.path.join(model_dir, model["templates"]))
    img = imread_u(screen_path, cv2.IMREAD_COLOR)
    full_h, full_w = img.shape[:2]

    if region:
        rx, ry, rw, rh = (int(v) for v in region)
        rx = max(0, min(rx, full_w - 1))
        ry = max(0, min(ry, full_h - 1))
        rw = max(1, min(rw, full_w - rx))
        rh = max(1, min(rh, full_h - ry))
    else:
        rx, ry, rw, rh = 0, 0, full_w, full_h

    # 边缘图整屏算一次(裁剪只影响 NCC 的搜索范围), 这样候选坐标与倒角距离表天然同坐标系
    edge_full = _edges(_gray_of(img))
    edge = edge_full[ry:ry + rh, rx:rx + rw]
    thr = float(score if score is not None else model["score"])
    stage1 = float(model.get("stage1_ncc", STAGE1_NCC))
    ncc_min = float(model.get("ncc_min", NCC_MIN))
    vmin = float(model.get("verify_min", VERIFY_MIN))
    tau_frac = float(model.get("verify_tau", VERIFY_TAU))

    cands = []
    for it in model["items"]:
        if labels and it["label"] not in labels:
            continue
        tmpl = bank[it["key"]].astype(np.float32)
        th, tw = tmpl.shape
        if th > edge.shape[0] or tw > edge.shape[1]:
            continue
        res = cv2.matchTemplate(edge, tmpl, cv2.TM_CCOEFF_NORMED)
        if float(res.max()) < stage1:          # 绝大多数"模板×位置"根本没有候选 ⇒ 先扫一遍最大值,
            continue                           # 省掉连通域分析(实测占第二段以外的一半耗时)
        for val, px, py in _peaks(res, stage1):
            if val < ncc_min:                 # 闸门一: 形状相关性不达标的候选直接丢, 连验证都不用做
                continue
            cands.append({"label": it["label"], "size": it["size"], "ncc": val,
                          "key": it["key"],
                          "x": px + rx, "y": py + ry, "w": tw, "h": th,
                          "cx": px + rx + tw / 2.0, "cy": py + ry + th / 2.0})

    # 第二段: 倒角距离表(整屏算一次, 只在候选点上取值 ⇒ 便宜)
    if cands:
        hard = (edge_full >= EDGE_BIN_HI).astype(np.uint8)
        dt = cv2.distanceTransform(1 - hard, cv2.DIST_L2, 3)
        verified = []
        for c in cands:
            tmpl = bank[c["key"]].astype(np.float32)
            v = _verify(dt, tmpl, c["x"], c["y"], tau_frac * max(c["w"], c["h"]))
            if v < vmin:                      # 闸门二: 几何对不上
                continue
            c["verify"] = v
            c["score"] = min(c["ncc"], v)     # 分数取两者里差的那个: 有一项弱就不算好命中
            if c["score"] >= thr:
                verified.append(c)
        cands = verified

    hits = []
    for c in _nms(cands, model["nms_iou"]):
        hits.append({
            "label": c["label"],
            "score": round(c["score"], 4),
            "ncc": round(c.get("ncc", 0.0), 4),
            "verify": round(c.get("verify", 0.0), 4),
            "rect": {"x": c["x"], "y": c["y"], "w": c["w"], "h": c["h"]},
            "nrect": {"x": round(c["x"] / full_w, 6), "y": round(c["y"] / full_h, 6),
                      "w": round(c["w"] / full_w, 6), "h": round(c["h"] / full_h, 6)},
            "center": {"x": round(c["cx"], 2), "y": round(c["cy"], 2)},
            "size": c["size"],
        })
    hits.sort(key=lambda h: -h["score"])
    return hits


# ---------------------------------------------------------------------------
# 合成素材(自测/回归用): 不引入外部图片, 图标用矢量图元画
# ---------------------------------------------------------------------------
def _icon_rgba(name: str, size: int = 32):
    """32 网格上画一个白色图标(alpha 底), 再按 size 缩放。"""
    img = np.zeros((32, 32, 4), np.uint8)
    w = (255, 255, 255, 255)
    if name == "ring":
        cv2.circle(img, (16, 16), 12, w, 3, cv2.LINE_AA)
        cv2.circle(img, (16, 16), 4, w, -1, cv2.LINE_AA)
    elif name == "star":
        pts = []
        for i in range(10):
            ang = -math.pi / 2 + i * math.pi / 5
            r = 13.0 if i % 2 == 0 else 5.5
            pts.append([16 + r * math.cos(ang), 16 + r * math.sin(ang)])
        cv2.fillPoly(img, [np.array(pts, np.int32)], w, cv2.LINE_AA)
    elif name == "arrow":
        cv2.rectangle(img, (5, 13), (19, 19), w, -1)
        cv2.fillPoly(img, [np.array([[18, 6], [28, 16], [18, 26]], np.int32)], w, cv2.LINE_AA)
    elif name == "plus":
        cv2.rectangle(img, (13, 4), (19, 28), w, -1)
        cv2.rectangle(img, (4, 13), (28, 19), w, -1)
    elif name == "folder":
        cv2.rectangle(img, (3, 8), (14, 14), w, -1)
        cv2.rectangle(img, (3, 11), (29, 26), w, -1)
        cv2.rectangle(img, (7, 16), (25, 20), (0, 0, 0, 0), -1)   # 内部挖一条透明缝
    elif name == "trash":
        cv2.rectangle(img, (12, 3), (20, 8), w, -1)
        cv2.rectangle(img, (8, 7), (24, 11), w, -1)
        cv2.rectangle(img, (10, 11), (22, 29), w, 3, cv2.LINE_AA)
    else:
        raise ValueError("unknown icon: %s" % name)
    if size != 32:
        img = cv2.resize(img, (size, size), interpolation=cv2.INTER_AREA)
    return img


def generate_icons(out_dir: str, names=ICON_NAMES) -> "list":
    os.makedirs(out_dir, exist_ok=True)
    paths = []
    for name in names:
        p = os.path.join(out_dir, "%s.png" % name)
        imwrite_u(p, _icon_rgba(name))
        paths.append(p)
    return paths


def generate_scene(icons_dir: str, out_dir: str, seed: int = 1, theme: str = "light",
                   width: int = 960, height: int = 600, names=ICON_NAMES,
                   scales=(0.7, 0.85, 1.0, 1.15, 1.3, 1.6)) -> dict:
    """合成一张"界面截图": 渐变背景 + 干扰面板/线条 + 若干图标(含同类重复、随机缩放)。
    返回 GT(每项的 label 与精确矩形)。同 seed + 同 theme ⇒ 输出完全一致(可复现)。"""
    rng = np.random.default_rng(seed)
    base = float(rng.integers(185, 245) if theme == "light" else rng.integers(20, 66))
    grad = np.linspace(-0.10, 0.10, height, dtype=np.float32)[:, None]
    col = np.repeat(np.clip(base * (1.0 + grad), 0, 255).astype(np.uint8), width, axis=1)
    img = np.dstack([col] * 3).copy()

    for _ in range(6):                                    # 干扰面板
        x, y = int(rng.integers(0, width - 140)), int(rng.integers(0, height - 90))
        w_, h_ = int(rng.integers(70, 200)), int(rng.integers(40, 120))
        shade = int(np.clip(base + rng.choice([-30, 30]), 0, 255))
        cv2.rectangle(img, (x, y), (x + w_, y + h_), (shade,) * 3, -1)
    # 干扰线条: 真实界面里的分隔线/边框那种样子(细、短、对比温和)。
    # 别放粗的高对比斜线 —— 那对任何形状匹配都是不合理对抗, 测出来的阈值没有参考价值。
    for _ in range(8):
        x, y = int(rng.integers(0, width - 40)), int(rng.integers(0, height - 40))
        dx = int(rng.choice([-1, 0, 1])) * int(rng.integers(10, 34))
        dy = int(rng.choice([-1, 0, 1])) * int(rng.integers(10, 34))
        shade = int(np.clip(base + rng.choice([-28, 28]), 0, 255))
        cv2.line(img, (x, y), (x + dx, y + dy), (shade,) * 3, 1, cv2.LINE_AA)
    img = cv2.GaussianBlur(img, (3, 3), 0)
    img = np.clip(img.astype(np.int16) + rng.integers(-3, 4, img.shape), 0, 255).astype(np.uint8)

    order = list(names) + [str(rng.choice(names))]        # 多放一个同类图标 ⇒ 考"同屏多实例"
    rng.shuffle(order)
    fg = 28 if theme == "light" else 238                  # 与背景形成明显对比
    boxes, items = [], []
    for name in order:
        size = int(round(32 * float(rng.choice(scales))))
        icon = _icon_rgba(name, size)
        for _try in range(300):
            x = int(rng.integers(4, max(5, width - size - 4)))
            y = int(rng.integers(4, max(5, height - size - 4)))
            if all(x + size + 6 <= bx or bx + bw + 6 <= x or y + size + 6 <= by or by + bh + 6 <= y
                   for bx, by, bw, bh in boxes):
                break
        else:
            continue                                      # 挤不下就跳过(数量不是断言目标)
        boxes.append((x, y, size, size))
        # alpha 混合(不要用二值掩码): 真实截图里的图标是抗锯齿的, 抹掉 AA 会让细线图标(圆环)对不上模板
        a = icon[:, :, 3:4].astype(np.float32) / 255.0
        patch = img[y:y + size, x:x + size].astype(np.float32)
        img[y:y + size, x:x + size] = np.clip(fg * a + patch * (1.0 - a), 0, 255).astype(np.uint8)
        items.append({"label": name, "rect": {"x": x, "y": y, "w": size, "h": size}})

    os.makedirs(out_dir, exist_ok=True)
    shot = os.path.join(out_dir, "screen_s%d_%s.png" % (seed, theme))
    imwrite_u(shot, img)
    gt = {"screen": shot, "width": width, "height": height, "seed": seed,
          "theme": theme, "items": items}
    with open(os.path.join(out_dir, "gt_s%d_%s.json" % (seed, theme)), "w", encoding="utf-8") as f:
        json.dump(gt, f, ensure_ascii=False, indent=1)
    return gt


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _selftest(out_dir: str, score: float) -> int:
    icons = os.path.join(out_dir, "icons")
    model = os.path.join(out_dir, "model")
    generate_icons(icons)
    m = build_model(icons, model, score=score)
    log("[icon_finder] model: labels=%s sizes=%d items=%d -> %s"
        % (",".join(m["labels"]), len(m["sizes"]), len(m["items"]), model))
    total_hit, total_gt, total_fp = 0, 0, 0
    for theme in ("light", "dark"):
        for seed in (1, 2):
            gt = generate_scene(icons, out_dir, seed=seed, theme=theme)
            t0 = time.time()
            hits = find_icons(model, gt["screen"])
            dt = (time.time() - t0) * 1000.0
            total_gt += len(gt["items"])
            total_hit += len(hits)
            log("[icon_finder] scene %s/seed=%d gt=%d hits=%d t=%.0fms"
                % (theme, seed, len(gt["items"]), len(hits), dt))
            for h in hits[:4]:
                log("    %-7s score=%.3f center=(%.0f,%.0f)" % (h["label"], h["score"],
                    h["center"]["x"], h["center"]["y"]))
    total_fp = max(0, total_hit - total_gt)
    log("[icon_finder] selftest gt=%d hits=%d (超额=%d) —— 未做严格判定, 严格判定在 test_icon_find.py"
        % (total_gt, total_hit, total_fp))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="icon finder (opencv backend)")
    sub = ap.add_subparsers(dest="cmd")

    b = sub.add_parser("build", help="模板集 -> 图标库模型")
    b.add_argument("--icons", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--backend", default="opencv")
    b.add_argument("--score", type=float, default=DEFAULT_SCORE)

    f = sub.add_parser("find", help="截图 + 模型 -> 命中列表")
    f.add_argument("--model", required=True)
    f.add_argument("--screen", required=True)
    f.add_argument("--region", default="", help="像素区域 x,y,w,h")
    f.add_argument("--score", type=float, default=None)
    f.add_argument("--json", default="", help="结果写到这里(否则打 stdout)")

    s = sub.add_parser("selftest", help="合成场景自测")
    s.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "jz_icon_finder_demo"))
    s.add_argument("--score", type=float, default=DEFAULT_SCORE)

    args = ap.parse_args(argv)
    if args.cmd == "build":
        try:
            m = build_model(args.icons, args.out, args.backend, score=args.score)
        except NotImplementedError as exc:
            log("[icon_finder] %s" % exc)
            return 3
        log("[icon_finder] build ok: %d items, labels=%s" % (len(m["items"]), ",".join(m["labels"])))
        return 0
    if args.cmd == "find":
        region = tuple(int(v) for v in args.region.split(",")) if args.region else None
        hits = find_icons(args.model, args.screen, region=region, score=args.score)
        text = json.dumps({"hits": hits}, ensure_ascii=False, indent=1)
        if args.json:
            with open(args.json, "w", encoding="utf-8") as fp:
                fp.write(text)
        log(text)
        return 0
    if args.cmd == "selftest":
        return _selftest(args.out, args.score)
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
