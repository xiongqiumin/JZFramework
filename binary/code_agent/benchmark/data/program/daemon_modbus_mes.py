#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
program_modbus_alarm_mes 用例的后台服务（daemon）。

它是"被测程序的另一侧"，一个进程里同时起两个服务：
  1) Modbus TCP 从站（模拟下位机/仪表）——只实现被测任务需要的 FC3 读保持寄存器；
  2) MES 服务端（模拟上位系统）——收 POST /alarm，把每条上报追加落盘成 jsonl。

为什么必须由 daemon 承担：Modbus 从站与 MES 都不是被测产物的一部分，模型写的
TS 程序只是一个"主站/客户端"，它需要有一台真实的下位机和一个真实的上位系统可连。
daemon 在每个用例期间常驻（宿主 finishCase 时随用例一起被杀掉），因此端口、数据、
上报记录天然按用例隔离。

本脚本自己决定参数形态（宿主不强加 --port），只约定：
  --base-port <N>   : Modbus 用 N，MES HTTP 用 N+1（也可 --modbus-port/--mes-port 单独覆盖）
  --out <文件>      : 写出本轮真值元数据 daemon.json；MES 收到的上报写在同目录 mes_alarms.jsonl
启动成功（两个端口都 listen 完）后打印一行 ready —— 宿主据此判定 daemon 已就绪。

退出：本进程由宿主 kill（Windows 下 JZProcess 的 Job Object 会带走子进程），无需自退。
"""
import argparse
import json
import os
import socketserver
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

#  UTF-8 输出保险：Windows 下 python 默认按 locale(GBK) 编码 stdout
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---- 本轮下位机的真值（被 verify 读取，不写进 prompt，避免模型照抄）----
UNIT_ID = 1
REGISTERS = [752, 823, 610, 905, 788]   # 保持寄存器 0..4：5 个温度测点，单位 0.1℃
THRESHOLD = 800                          # 告警阈值（原始值 > 800 即告警）
HOLDING_BASE = 0


def expected_alarms():
    """真值告警清单：[(寄存器地址, 原值), ...] —— 只有超过阈值的测点"""
    return [(HOLDING_BASE + i, v) for i, v in enumerate(REGISTERS) if v > THRESHOLD]


class ModbusHandler(socketserver.BaseRequestHandler):
    """最小 Modbus TCP 从站：只读构造器（FC3），够用即止。"""

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.request.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("peer closed")
            buf += chunk
        return buf

    def handle(self):
        conn = self.request
        try:
            conn.settimeout(60)
        except Exception:
            pass
        while True:
            try:
                header = self.recv_exact(7)             # MBAP: tid(2) pid(2) len(2) uid(1)
            except Exception:
                return
            tid, pid, length, uid = struct.unpack(">HHHB", header)
            body_len = length - 1                       # length 含 unit id 那一字节
            if pid != 0 or body_len <= 0 or body_len > 256:
                return
            try:
                pdu = self.recv_exact(body_len)
            except Exception:
                return

            fc = pdu[0]
            if fc == 0x03 and len(pdu) >= 5:
                addr, qty = struct.unpack(">HH", pdu[1:5])
                vals = []
                ok = 0 < qty <= 125
                for i in range(qty):
                    idx = addr - HOLDING_BASE + i
                    if 0 <= idx < len(REGISTERS):
                        vals.append(REGISTERS[idx])
                    else:
                        ok = False
                        break
                if ok:
                    data = struct.pack(">BB", 0x03, qty * 2)
                    data += b"".join(struct.pack(">H", v) for v in vals)
                else:
                    data = struct.pack(">BB", 0x83, 0x02)     # 非法数据地址
            else:
                data = struct.pack(">BB", fc | 0x80, 0x01)    # 非法功能码

            resp = struct.pack(">HHHB", tid, 0, len(data) + 1, uid) + data
            try:
                conn.sendall(resp)
            except Exception:
                return


class MesHandler(BaseHTTPRequestHandler):
    """最小 MES 接收端：POST /alarm -> 落盘一行 jsonl -> 200 {"ok":true}"""

    alarms_path = "mes_alarms.jsonl"

    def do_POST(self):
        path = self.path.split("?")[0]
        if path != "/alarm":
            self.send_error(404)
            return
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except Exception:
            n = 0
        raw = self.rfile.read(n) if n > 0 else b""
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            obj = {"_raw": raw.decode("utf-8", "replace")}
        obj["_recv_ts"] = int(time.time() * 1000)
        try:
            with open(MesHandler.alarms_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(obj, ensure_ascii=False) + "\n")
                f.flush()
        except Exception as e:
            self.send_error(500, "write failed: %s" % e)
            return
        body = b'{"ok":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    def do_GET(self):
        """排错用：GET /alarms 返回已收到的条数"""
        body = json.dumps({"count": len(list_alarms(MesHandler.alarms_path))}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass        # 静音访问日志，免得把管道塞满


def list_alarms(path):
    out = []
    if not os.path.isfile(path):
        return out
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except Exception:
                    out.append({"_raw": line})
    except Exception:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-port", type=int, default=3900,
                    help="Modbus 端口 N, MES HTTP 用 N+1")
    ap.add_argument("--modbus-port", type=int, default=0, help="单独指定 Modbus 端口(覆盖 base)")
    ap.add_argument("--mes-port", type=int, default=0, help="单独指定 MES 端口(覆盖 base+1)")
    ap.add_argument("--out", default="daemon.json", help="真值元数据写出路径")
    args = ap.parse_args()

    modbus_port = args.modbus_port or args.base_port
    mes_port = args.mes_port or (args.base_port + 1)

    out_path = os.path.abspath(args.out)
    out_dir = os.path.dirname(out_path) or "."
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    alarms_file = os.path.join(out_dir, "mes_alarms.jsonl")
    # 每个用例一个 daemon 实例 -> 清空上一轮残留, 保证上报记录只属于本轮
    with open(alarms_file, "w", encoding="utf-8"):
        pass
    MesHandler.alarms_path = alarms_file

    meta = {
        "modbus_host": "127.0.0.1",
        "modbus_port": modbus_port,
        "mes_port": mes_port,
        "mes_alarm_url": "http://127.0.0.1:%d/alarm" % mes_port,
        "unit_id": UNIT_ID,
        "registers": REGISTERS,
        "threshold": THRESHOLD,
        "expected_alarms": [{"point": p, "value": v} for p, v in expected_alarms()],
        "alarms_file": alarms_file,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    try:
        modbus = socketserver.ThreadingTCPServer(("127.0.0.1", modbus_port), ModbusHandler)
        mes = ThreadingHTTPServer(("127.0.0.1", mes_port), MesHandler)
    except Exception as e:
        print("daemon bind failed: %s" % e)
        return 1

    threading.Thread(target=modbus.serve_forever, daemon=True).start()
    threading.Thread(target=mes.serve_forever, daemon=True).start()

    print("ready modbus=%d mes=%d out=%s" % (modbus_port, mes_port, out_path), flush=True)
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
