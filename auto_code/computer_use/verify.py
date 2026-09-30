#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
computer_use 脚手架 —— 断言 / 判分器

这不是"评测"：评测只给分数，脚手架要给"下一步改哪里"。
所以失败时打印**逐节点 diff** —— 它会被原样喂回模型当下一轮的输入。

用法（在本目录下跑）:
  python verify.py                      # 判分: scenes/truth.json vs runs/current/tree.json
  python verify.py --truth A --pred B   # 指定文件
  python verify.py --check-only         # 只打印结论、不写产物（给外部复验用）
  python verify.py --self-test          # 尺子自检: 真值当预测必须满分, 篡改必须失败

输入格式（真值与预测同一套）:
  {"nodes": [ {"id":"n1","type":"button","text":"7","rect":[10,60,40,30],"parent":"n0"} ]}
  rect = [x, y, w, h] 逻辑像素;  parent = 父节点 id（根为 null）

硬门: 五项齐过才 PASS（不做加权总分 —— 权重会掩盖短板）。
判定标记只用 ASCII（PASS / FAIL / GATE / [loop-stop]）。
"""

import argparse
import json
import os
import sys
import unicodedata

try:
    # 往管道写中文要自己转 UTF-8（jzInfo 那侧是 GBK，脚本侧得自己声明）
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TRUTH = os.path.join(HERE, "scenes", "truth.json")
DEFAULT_PRED = os.path.join(HERE, "runs", "current", "tree.json")
DEFAULT_RESULT = os.path.join(HERE, "runs", "current", "result.json")

# 硬门（必须与 baseline.json 的 gates 一致；改口径要两边一起改）
GATES = {
    "detF1": 0.90,
    "textAcc": 0.95,
    "typeAcc": 0.90,
    "hierF1": 0.85,
    "iou75": 0.60,
}

# 类型等价族：真值是"控件语义类"，预测可能给"视觉类" —— 同族即算对。
# 加新词汇只往这里加，别在判分逻辑里写 if。
TYPE_FAMILY = {
    "app": "container", "window": "container", "group": "container",
    "toolbar": "container", "panel": "container", "frame": "container",
    "tab": "tab", "tabwidget": "tab",
    "menu": "menu", "menubar": "menu", "menuitem": "menu",
    "button": "button", "link": "button", "icon": "button", "pushbutton": "button",
    "label": "text", "text": "text", "textblock": "text", "static": "text",
    "edit": "input", "textbox": "input", "textedit": "input", "lineedit": "input",
    "combobox": "input", "spinbox": "input", "searchbox": "input",
    "checkbox": "check", "switch": "check", "radio": "check", "radiobutton": "check",
    "slider": "slider", "progressbar": "progress", "progress": "progress",
    "list": "list", "listwidget": "list",
    "table": "table", "tablewidget": "table",
    "tree": "tree", "treewidget": "tree",
    "canvas": "canvas", "image": "image", "scrollbar": "scrollbar",
}


def fam(t):
    t = (t or "").lower()
    return TYPE_FAMILY.get(t, t or "unknown")


def norm_text(s):
    """文本归一化：Unicode NFKC（全角→半角，以及 ℃→°C / ㎡→m2 这类"可兼容等价"）
    → 压内部空白、去首尾 → 统一小写。

    ⚠ 用 NFKC 而不是手写"全角→半角"表：℃(U+2103) 这类字符的规范等价就是 "°C"，
    模型把它写成 "°C" 完全正确，手写表会误判成"文本不符"（实测踩过）。"""
    return " ".join(unicodedata.normalize("NFKC", s or "").split()).lower()


def _rect(r):
    r = list(r or [])
    while len(r) < 4:
        r.append(0)
    try:
        return [float(v) for v in r[:4]]
    except (TypeError, ValueError):
        return [0.0, 0.0, 0.0, 0.0]


def _nodes_from_doc(doc):
    raw = doc.get("nodes") if isinstance(doc, dict) else doc
    nodes = []
    for i, n in enumerate(raw or []):
        n = n or {}
        pid = n.get("parent")
        nodes.append({
            "id": str(n.get("id", "#%d" % i)),
            "type": (n.get("type") or "").lower(),
            "text": n.get("text") or "",
            "rect": _rect(n.get("rect")),
            "parent": None if pid in (None, "") else str(pid),
        })
    return nodes


def load_nodes(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return _nodes_from_doc(json.load(f))


def iou(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    if aw <= 0 or ah <= 0 or bw <= 0 or bh <= 0:
        return 0.0
    x1, y1 = max(ax, bx), max(ay, by)
    x2, y2 = min(ax + aw, bx + bw), min(ay + ah, by + bh)
    iw, ih = x2 - x1, y2 - y1
    if iw <= 0 or ih <= 0:
        return 0.0
    inter = iw * ih
    return inter / (aw * ah + bw * bh - inter)


def _median(xs):
    if not xs:
        return 0.0
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def _edge_f1(truth, pred, t2p):
    """父子边集合的 F1。

    - 只在"两端都匹配上"的真值边上比较（真值父节点没被找到时，这条边无从判定，跳过）；
    - ⚠ 一条边都判不了时**不给分**（0.0）：否则"预测全是一层平铺、一个父都不给"会白拿 1.0。
    """
    t_idx, p_idx = {n["id"]: i for i, n in enumerate(truth)}, {n["id"]: i for i, n in enumerate(pred)}
    truth_has_edge = any(n["parent"] is not None for n in truth)
    te, pe = set(), set()
    for ti, pi in t2p.items():
        tp = truth[ti]["parent"]
        if tp is not None:
            if t_idx.get(tp) not in t2p:
                continue                                  # 真值父节点未匹配 -> 不算这条边
            t_parent = t2p[t_idx[tp]]
        else:
            t_parent = None
        pp = pred[pi]["parent"]
        p_parent = p_idx.get(pp) if pp is not None else None
        te.add((t_parent, pi))
        pe.add((p_parent, pi))
    if not te and not pe:
        return (0.0 if truth_has_edge else 1.0), te, pe
    inter = te & pe
    prec = len(inter) / len(pe) if pe else 0.0
    rec = len(inter) / len(te) if te else 0.0
    f1 = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
    return f1, te, pe


def score(truth, pred, iou_th=0.5):
    cand = []
    for i, t in enumerate(truth):
        for j, p in enumerate(pred):
            v = iou(t["rect"], p["rect"])
            if v >= iou_th:
                cand.append((v, i, j))
    cand.sort(key=lambda x: -x[0])

    used_t, used_p, matched = set(), set(), []
    for v, i, j in cand:
        if i in used_t or j in used_p:
            continue
        used_t.add(i)
        used_p.add(j)
        matched.append((i, j, v))

    missed = [(i, truth[i]) for i in range(len(truth)) if i not in used_t]
    spurious = [(j, pred[j]) for j in range(len(pred)) if j not in used_p]

    n_m = len(matched)
    prec = n_m / len(pred) if pred else 0.0
    rec = n_m / len(truth) if truth else 0.0
    det_f1 = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0

    bad_text = [(i, j) for i, j, _ in matched if norm_text(truth[i]["text"]) != norm_text(pred[j]["text"])]
    bad_type = [(i, j) for i, j, _ in matched if fam(truth[i]["type"]) != fam(pred[j]["type"])]
    ious = [v for _, _, v in matched]
    hier_f1, _, _ = _edge_f1(truth, pred, {i: j for i, j, _ in matched})

    metrics = {
        "detP": round(prec, 4), "detR": round(rec, 4), "detF1": round(det_f1, 4),
        "textAcc": round((n_m - len(bad_text)) / n_m, 4) if n_m else 0.0,
        "typeAcc": round((n_m - len(bad_type)) / n_m, 4) if n_m else 0.0,
        "hierF1": round(hier_f1, 4),
        "iouMed": round(_median(ious), 4),
        "iou75": round(len([v for v in ious if v >= 0.75]) / n_m, 4) if n_m else 0.0,
    }
    gates = [(("%s=%.3f>=%.2f" % (k, metrics[k], GATES[k])), metrics[k] >= GATES[k]) for k in GATES]
    return {
        "counts": {"truth": len(truth), "pred": len(pred), "matched": n_m,
                   "missed": len(missed), "spurious": len(spurious)},
        "metrics": metrics,
        "gates": gates,
        "pass": all(ok for _, ok in gates),
        "detail": {"missed": missed, "spurious": spurious,
                   "badText": bad_text, "badType": bad_type},
    }


def _fail_lines(res, limit=20):
    """给人/给模型的 diff。写清"错在哪 + 真值是什么"，因为它就是下一轮的 prompt。"""
    lines, d = [], res["detail"]
    if d["missed"]:
        lines.append("漏检 %d 个（真值有、预测没有）:" % len(d["missed"]))
        for _, t in d["missed"][:limit]:
            lines.append('  - [%s] %s "%s" rect=%s' % (t["id"], t["type"], t["text"], t["rect"]))
        if len(d["missed"]) > limit:
            lines.append("  ... 还有 %d 个" % (len(d["missed"]) - limit))
    if d["spurious"]:
        lines.append("虚警 %d 个（预测多报的）:" % len(d["spurious"]))
        for _, p in d["spurious"][:limit]:
            lines.append('  + [%s] %s "%s" rect=%s' % (p["id"], p["type"], p["text"], p["rect"]))
        if len(d["spurious"]) > limit:
            lines.append("  ... 还有 %d 个" % (len(d["spurious"]) - limit))
    if d["badText"]:
        lines.append("文本不符 %d 个（位置对上了、字不对）:" % len(d["badText"]))
        for i, j in d["badText"][:limit]:
            lines.append('  ~ 期望"%s" 实际"%s" rect=%s' % (res["_truth"][i]["text"], res["_pred"][j]["text"],
                                                            res["_truth"][i]["rect"]))
    if d["badType"]:
        lines.append("类型不符 %d 个（同族才算对）:" % len(d["badType"]))
        for i, j in d["badType"][:limit]:
            lines.append('  ~ 期望=%s(族:%s) 实际=%s(族:%s) text="%s"' % (
                res["_truth"][i]["type"], fam(res["_truth"][i]["type"]),
                res["_pred"][j]["type"], fam(res["_pred"][j]["type"]), res["_truth"][i]["text"]))
    return lines


def run(truth_path, pred_path, check_only):
    truth, pred = load_nodes(truth_path), load_nodes(pred_path)
    res = score(truth, pred)
    res["_truth"], res["_pred"] = truth, pred
    res["paths"] = {"truth": truth_path, "pred": pred_path}

    m, c = res["metrics"], res["counts"]
    print("truth=%d pred=%d matched=%d" % (c["truth"], c["pred"], c["matched"]))
    print("detF1=%.3f (P=%.3f R=%.3f)  textAcc=%.3f  typeAcc=%.3f  hierF1=%.3f  iouMed=%.3f  iou75=%.3f"
          % (m["detF1"], m["detP"], m["detR"], m["textAcc"], m["typeAcc"], m["hierF1"], m["iouMed"], m["iou75"]))

    if not res["pass"] and not check_only:
        print("--- diff（这就是下一轮的输入）---")
        for line in _fail_lines(res, limit=20):
            print(line)
        print("---")
    for name, ok in res["gates"]:
        if not ok:
            print("GATE FAIL: %s" % name)

    print("PASS" if res["pass"] else "FAIL")
    if res["pass"]:
        print("[loop-stop]")

    if not check_only:
        out = {k: v for k, v in res.items() if not k.startswith("_")}
        try:
            os.makedirs(os.path.dirname(DEFAULT_RESULT), exist_ok=True)
            with open(DEFAULT_RESULT, "w", encoding="utf-8") as f:
                json.dump(out, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print("WARN: 写 result.json 失败: %s" % e)
    return 0 if res["pass"] else 1


def self_test():
    """尺子自检：① 真值当预测 -> 必须满分 PASS；② 篡改几处 -> 必须 FAIL 且报出对应类别。"""
    truth = {"nodes": [
        {"id": "n0", "type": "window", "text": "计算器", "rect": [0, 0, 400, 300], "parent": None},
        {"id": "n1", "type": "button", "text": "7", "rect": [10, 60, 40, 30], "parent": "n0"},
        {"id": "n2", "type": "button", "text": "8", "rect": [60, 60, 40, 30], "parent": "n0"},
        {"id": "n3", "type": "edit", "text": "", "rect": [10, 20, 120, 30], "parent": "n0"},
        {"id": "n4", "type": "label", "text": "结果", "rect": [140, 20, 60, 30], "parent": "n0"},
    ]}
    tn = _nodes_from_doc(truth)

    a = score(tn, tn)
    ok1 = (a["pass"] and all(a["metrics"][k] == 1.0 for k in ("detF1", "textAcc", "typeAcc", "hierF1", "iou75")))
    print("self-test 1 真值 vs 真值  -> %s  %s" % ("OK" if ok1 else "FAIL", a["metrics"]))

    bad = {"nodes": [
        {"id": "p0", "type": "window", "text": "计算器", "rect": [0, 0, 400, 300], "parent": None},
        {"id": "p1", "type": "link", "text": "7", "rect": [12, 61, 40, 30], "parent": "p0"},     # 同族 -> 算对
        {"id": "p2", "type": "button", "text": "9", "rect": [60, 60, 40, 30], "parent": "p0"},   # 文本不符
        {"id": "p3", "type": "checkbox", "text": "", "rect": [10, 20, 120, 30], "parent": None}, # 类型不符 + 层级不符
        {"id": "p5", "type": "button", "text": "退出", "rect": [300, 260, 60, 30], "parent": "p0"},  # 虚警
        # n4(label "结果") 漏检
    ]}
    b = score(tn, _nodes_from_doc(bad))
    kinds = (len(b["detail"]["missed"]), len(b["detail"]["spurious"]),
             len(b["detail"]["badText"]), len(b["detail"]["badType"]))
    ok2 = (not b["pass"]) and kinds == (1, 1, 1, 1) and b["metrics"]["hierF1"] < 1.0
    print("self-test 2 真值 vs 篡改  -> %s  missed/spur/badText/badType=%s hierF1=%.3f"
          % ("OK" if ok2 else "FAIL", kinds, b["metrics"]["hierF1"]))
    if not ok2:
        for line in _fail_lines(b if b.get("_truth") else dict(b, _truth=tn, _pred=_nodes_from_doc(bad))):
            print(line)

    ok = ok1 and ok2
    print("SELF-TEST %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="computer_use 脚手架断言")
    ap.add_argument("--truth", default=DEFAULT_TRUTH)
    ap.add_argument("--pred", default=DEFAULT_PRED)
    ap.add_argument("--check-only", action="store_true", help="只打印结论, 不写产物（外部复验用）")
    ap.add_argument("--self-test", action="store_true", help="尺子自检: 真值当预测必须满分")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    for p, label in ((args.truth, "truth"), (args.pred, "pred")):
        if not os.path.exists(p):
            print("FAIL: 找不到 %s 文件: %s" % (label, p))
            return 1
    return run(args.truth, args.pred, args.check_only)


if __name__ == "__main__":
    sys.exit(main())
