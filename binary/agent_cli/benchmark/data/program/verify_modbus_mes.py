#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
program_modbus_alarm_mes 的外部验证脚本。

被测产物（code_runtime, cwd=build）是一个"上位机采集程序"，它应该完成三件事：
  1) 用内置模块 modbus 做主站，读取 daemon 起的那台 Modbus TCP 从站的保持寄存器 0..4；
  2) 把采集到的每一个测点存进 SQLite 的 samples 表；
  3) 对超过阈值的测点（告警点）：a) 用 fetch POST 上报给 daemon 起的 MES 服务端，
     b) 同时存进 alarms 表。

所以本脚本完全不看 stdout，只查两个"外部世界的副作用"，并且期望值不写死在脚本里、
而是从 daemon 写下的 daemon.json 读 —— 真值只有 daemon 一处，模型翻 prompt 也抄不到。

  --db          比 samples / alarms 两张表的内容
  --mes         比 mes_alarms.jsonl 里 daemon 实际收到的上报

退出码：0=通过，1=未通过（stdout 作为模型重试反馈）。
"""
import argparse
import json
import os
import sqlite3
import subprocess
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def load_meta(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def same_pairs(rows, expect):
    """rows: [(point, value), ...]（可能有重复行） -> 去重后是否等于 expect 集合"""
    got = set()
    for p, v in rows:
        try:
            got.add((int(p), int(v)))
        except Exception:
            continue
    want = set((int(e["point"]), int(e["value"])) for e in expect)
    return got == want, got, want


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件路径")
    ap.add_argument("--build", required=True, help="模型产物构建目录（运行时 cwd）")
    ap.add_argument("--root", default="", help="用例工程根目录（daemon.json 在这里）")
    ap.add_argument("--daemon-file", default="", help="真值元数据路径（默认 <root>/daemon.json）")
    args = ap.parse_args()

    build = os.path.normpath(args.build)
    root = os.path.normpath(args.root) if args.root else ""
    meta_path = args.daemon_file or (os.path.join(root, "daemon.json") if root else "daemon.json")

    if not os.path.isfile(meta_path):
        print("daemon 元数据不存在: %s（daemon 没起来？）" % meta_path)
        return 1
    try:
        meta = load_meta(meta_path)
    except Exception as e:
        print("读取 daemon 元数据失败: %s: %s" % (meta_path, e))
        return 1

    registers = meta.get("registers", [])
    expected = meta.get("expected_alarms", [])
    alarms_file = meta.get("alarms_file", "")

    # ---- 跑模型产物（一次性程序：采一轮 -> 上报 -> 存盘 -> 退出）----
    js_out = ""
    try:
        proc = subprocess.Popen([args.runtime], cwd=build,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except Exception as e:
        print("无法启动 code_runtime: %s" % e)
        return 1
    try:
        out, _ = proc.communicate(timeout=25)
        js_out = out.decode("utf-8", "replace") if out else ""
    except Exception:
        proc.kill()
        js_out = js_out or ""
        print("code_runtime 超时(25s)，未收工就退出（检查是不是把监听/setInterval 挂住了）")
    if proc.returncode != 0:
        print("[CRASH] code_runtime 退出码 %d" % proc.returncode)
        if js_out.strip():
            print(js_out)
        return 1

    results = []

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        print("[%s] %s%s" % ("PASS" if ok else "FAIL", name, (": " + detail) if detail else ""))

    # ---- 1) SQLite：sides effect 必须对得上 daemon 的真值 ----
    db_path = os.path.join(build, "alarm.db")
    if not os.path.isfile(db_path):
        db_path = os.path.join(root, "alarm.db") if root else db_path
    check("sqlite_file", os.path.isfile(db_path), "期望 %s 存在" % db_path)

    if os.path.isfile(db_path):
        try:
            con = sqlite3.connect(db_path)
            cur = con.cursor()

            def rows_of(table):
                cur.execute('SELECT name FROM sqlite_master WHERE type=? AND name=?', ("table", table))
                if cur.fetchone() is None:
                    return None
                cur.execute("SELECT point, value FROM %s" % table)
                return cur.fetchall()

            samples = rows_of("samples")
            alarms = rows_of("alarms")

            if samples is None:
                check("samples_table", False, "缺少 samples 表")
            else:
                check("samples_rows", len(samples) >= len(registers),
                      "期望至少 %d 行, 实际 %d 行" % (len(registers), len(samples)))
                want_samples = [{"point": i, "value": v} for i, v in enumerate(registers)]
                ok, got, want = same_pairs(samples, want_samples)
                check("samples_content", ok,
                      "期望(去重)=%s 实际=%s" % (sorted(want), sorted(got)))

            if alarms is None:
                check("alarms_table", False, "缺少 alarms 表")
            else:
                ok, got, want = same_pairs(alarms, expected)
                check("alarms_content", ok, "期望告警点=%s 实际=%s" % (sorted(want), sorted(got)))

            con.close()
        except Exception as e:
            check("sqlite_read", False, "%s: %s" % (type(e).__name__, e))

    # ---- 2) MES：daemon 侧真的收到了告警上报 ----
    got_rows = []
    if alarms_file and os.path.isfile(alarms_file):
        try:
            with open(alarms_file, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        got_rows.append(json.loads(line))
                    except Exception:
                        pass
        except Exception as e:
            check("mes_file_read", False, str(e))
    check("mes_received", len(got_rows) > 0, "MES 收到 %d 条上报" % len(got_rows))

    pairs = []
    for obj in got_rows:
        if isinstance(obj, dict) and "point" in obj and "value" in obj:
            pairs.append((obj.get("point"), obj.get("value")))
    ok, got, want = same_pairs(pairs, expected)
    check("mes_report_content", ok, "期望上报告警点=%s 实际=%s" % (sorted(want), sorted(got)))

    failed = [r for r in results if not r[1]]
    if not results:
        print("RESULT: FAIL (无有效断言)")
        return 1
    if failed:
        print("RESULT: FAIL (%d/%d 失败)" % (len(failed), len(results)))
        if js_out.strip():
            print("---- 程序输出 ----")
            print(js_out.strip()[:2000])
        return 1
    print("RESULT: PASS (%d 断言全部通过)" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
