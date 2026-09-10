#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
five_chess_server 的自动化校验程序（不需要 AI，固定用例）。

由 benchmark 通过 verify 命令拉起，负责：
  1) 用 code_runtime 启动模型生成的 server（cwd = 构建目录）；
  2) 等待端口就绪；起不来 -> 退出码 1，且只把 JS 进程自身的输出作为反馈（"产物崩溃"由 JS 进程层判定）；
  3) 跑一组固定断言：棋盘正确性 / 重复落子 / 错误数据 / 同点连落 / 正确终局；
  4) 全部通过 -> 0；断言失败（含 server 崩溃）-> 1。
退出码约定：0 = 通过，1 = 未通过（stdout 作为模型重试反馈）。

协议见 data/five_chess_server/five_chess_server.json 中 five_chess_server 的 prompt。
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

PORT_DEFAULT = 3900
HOST = "127.0.0.1"


def http_get(base, path):
    url = base + path
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def http_post(base, path, body):
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        base + path, data=data,
        headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read().decode("utf-8"))


def wait_for_port(base, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            http_get(base, "/list_rooms")
            return True
        except Exception:
            time.sleep(0.3)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runtime", required=True, help="code_runtime 可执行文件路径")
    ap.add_argument("--build", required=True, help="模型产物构建目录（server 工作目录）")
    ap.add_argument("--port", type=int, default=PORT_DEFAULT)
    ap.add_argument("--host", default=HOST)
    args = ap.parse_args()

    base = "http://%s:%d" % (args.host, args.port)
    results = []  # (name, ok, detail)

    def check(name, ok, detail=""):
        results.append((name, ok, detail))
        tag = "PASS" if ok else "FAIL"
        print("[%s] %s%s" % (tag, name, (": " + detail) if detail else ""))

    # ---- 启动模型 server ----
    print("启动 server: %s (cwd=%s)" % (args.runtime, args.build))
    proc = subprocess.Popen([args.runtime], cwd=args.build,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        if not wait_for_port(base):
            # JS 进程自身未能启动 / 异常退出：只返回 JS 进程自身的输出作为重试反馈（退出码 1）
            out = b""
            try:
                out = proc.communicate(timeout=2)[0] or b""
            except Exception:
                pass
            js_out = out.decode("utf-8", "replace")
            if js_out.strip():
                print(js_out)   # 仅返回 JS 进程自身输出
            else:
                print("(server 进程启动后无任何输出，端口 %d 在限定时间内未就绪)" % args.port)
            return 1

        # ---------- 1) 建房间 + 加入 ----------
        try:
            cr = http_post(base, "/create_room", {"name": "verify_room"})
            assert cr.get("room_id") is not None, "create_room 无 room_id"
            assert cr.get("token"), "create_room 无 token"
            room_id = int(cr["room_id"])
            black = cr["token"]
        except Exception as e:
            check("create_room", False, str(e)); 
            return finish(proc, results)

        try:
            jr = http_post(base, "/join_room", {"room_id": room_id})
            assert jr.get("token"), "join_room 无 token"
            white = jr["token"]
        except Exception as e:
            check("join_room", False, str(e)); 
            return finish(proc, results)

        # ---------- 2) 棋盘正确性 ----------
        # 黑下 (7,7)，get_match 应反映 board[y][7][x=7]==1 且轮到白
        try:
            pr = http_post(base, "/play",
                           {"room_id": room_id, "token": black, "x": 7, "y": 7})
            assert pr.get("ok") is True, "play(7,7) 应成功: %s" % pr
            gm = http_get(base, "/get_match?room_id=%d&token=%s" % (room_id, black))
            board = gm.get("board")
            assert isinstance(board, list) and len(board) == 15, "board 不是 15x15"
            assert board[7][7] == 1, "落子(7,7)后 board[y=7][x=7] 应为黑(1)，实际=%s" % board[7][7]
            assert gm.get("turn") == 1, "黑落子后应轮到白(turn=1)，实际=%s" % gm.get("turn")
            assert gm.get("move_count") == 1, "move_count 应为1，实际=%s" % gm.get("move_count")
            check("board_correctness", True)
        except Exception as e:
            check("board_correctness", False, str(e)); 
            return finish(proc, results)

        # ---------- 3) 重复落子（同点连落） ----------
        try:
            pr2 = http_post(base, "/play",
                            {"room_id": room_id, "token": black, "x": 7, "y": 7})
            assert pr2.get("ok") is False, "在已占点(7,7)重复落子应被拒绝 ok=false，实际=%s" % pr2
            check("repeat_placement", True)
        except Exception as e:
            check("repeat_placement", False, str(e)); 
            return finish(proc, results)

        # ---------- 4) 错误数据 ----------
        try:
            # 越界
            pr3 = http_post(base, "/play",
                            {"room_id": room_id, "token": black, "x": 99, "y": 0})
            assert pr3.get("ok") is False, "越界 x=99 应被拒绝，实际=%s" % pr3
            # 错误 token
            pr4 = http_post(base, "/play",
                            {"room_id": room_id, "token": "WRONG_TOKEN", "x": 5, "y": 5})
            assert pr4.get("ok") is False, "错误 token 应被拒绝，实际=%s" % pr4
            check("bad_data_rejected", True)
        except Exception as e:
            check("bad_data_rejected", False, str(e)); 
            return finish(proc, results)

        # ---------- 5) 正确终局（黑五连珠获胜） ----------
        try:
            cr2 = http_post(base, "/create_room", {"name": "endgame"})
            rid = int(cr2["room_id"]); bk = cr2["token"]
            jr2 = http_post(base, "/join_room", {"room_id": rid})
            wt = jr2["token"]
            black_moves = [(0, 0), (0, 1), (0, 2), (0, 3), (0, 4)]
            white_moves = [(1, 0), (1, 1), (1, 2), (1, 3)]
            last_result = 0
            for i in range(5):
                prb = http_post(base, "/play",
                                {"room_id": rid, "token": bk,
                                 "x": black_moves[i][0], "y": black_moves[i][1]})
                assert prb.get("ok") is True, "黑落子失败: %s" % prb
                last_result = prb.get("result", 0)
                if i < 4:
                    prw = http_post(base, "/play",
                                    {"room_id": rid, "token": wt,
                                     "x": white_moves[i][0], "y": white_moves[i][1]})
                    assert prw.get("ok") is True, "白落子失败: %s" % prw
            assert last_result == 1, "黑五连珠后 result 应为 1(黑胜)，实际=%s" % last_result
            check("game_termination_win", True)
        except Exception as e:
            check("game_termination_win", False, str(e)); 
            return finish(proc, results)

        return finish(proc, results)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except Exception:
                proc.kill()


def finish(proc, results):
    # 确保进程退出
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
    failed = [r for r in results if not r[1]]
    if not results:
        print("RESULT: FAIL (无有效断言)")
        return 1
    if failed:
        print("RESULT: FAIL (%d/%d 失败)" % (len(failed), len(results)))
        return 1
    print("RESULT: PASS (%d 断言全部通过)" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
