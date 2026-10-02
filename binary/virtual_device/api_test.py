#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
虚拟桌面容器 HTTP 接口的冒烟测试 (端到端, 真的起进程 + 真的走 socket)。

用法:
  python binary/virtual_device/api_test.py                # 自动找 Debug 的 exe
  python binary/virtual_device/api_test.py --cfg Release
  python binary/virtual_device/api_test.py --keep         # 测完不杀进程 (手工接着玩)
  python binary/virtual_device/api_test.py --exe <path>   # 指定 exe

测什么 (全部走 HTTP, 不碰进程内部):
  1. /api/health           容器起来了, 桌面 800x600, scale 1
  2. /api/screen           整屏 PNG = 桌面逻辑尺寸 (无头下 scale 恒 1)
  3. /api/windows POST     新建窗口: 几何 = 外框 / 客户区 (装饰尺寸差: border 2 / title 22)
  4. /api/programs POST    加载程序 (JS: 窗口 + 输入框 + 按钮, 点按钮把文本写进标题)
  5. /api/screen?window=   窗口客户区截图: PNG 尺寸 == 客户区逻辑尺寸
  6. 鼠标 + 键盘           点输入框 -> 打字 -> 点按钮 -> 窗口标题变成 "文本=hello"
  7. 拖动                  按标题栏拖 -> frame 位置真的变了 (窗口管理器那条路)
  8. /api/desktop scale=2  截图翻倍, 而坐标/几何一个都没动 (DPI 不进坐标)
  9. 程序                  用文件加载 (ESM 模块) / 停掉 -> 窗口一起收走
  9b. 程序异常退出        未处理异常 = 那个程序自己退出 (程序表消失 + 窗口收走); 起来就崩 -> 400
 10. 查找 / 枚举窗口       句柄 id / 下标 / 标题 三种标识, ?process= ?title= ?visible= 过滤,
                           at=x,y = WindowFromPoint (+ 客户区坐标)
 11. 直投窗口输入          给**后台窗口**发鼠标/键盘 (不激活它): 点输入框 -> 打字 -> 点按钮
                           -> 窗口标题变了, 而活动窗口全程没变; coords=screen 会内部换算
 12. 窗口状态              w/h (客户区) / x,y (外框) / title / 最小化 / 最大化铺工作区 / 还原 / 关闭
 12b. 关程序窗口           点**程序**窗口的关闭按钮 / DELETE /api/windows/<程序窗口>: 窗口关掉、
                          容器不退、程序还在; 之后停那个程序也不踩它缓存的野窗口指针
                          (这条以前直接把容器打崩: 主线程跨线程 delete 客户区)
 13. 错误路径              未知路由 404 / 坏 JSON 400 / 找不到的窗口与句柄 404 / 参数不对 400
 14. 收尾                  DELETE 程序 -> 它的窗口一起没了
 15. /api/logs            程序输出 (console.log 等) 的最近 1000 行: 按行取 (offset/size, -1=全部),
                          超 1000 条从头滚动, 行号绝对 ⇒ 记住 total 就能增量拉

为什么放这儿 (而不是 auto_code): 这是**容器自己**的接口测试, 跟任何 agent 任务无关;
它是"这个接口还能用吗"的那把尺子, 改完接口应该跑一遍。

⚠ 输入/关窗都是**投递**语义 (见 JZVirtualWindow::injectMouse / close):
  · 给**程序窗口**发东西 = 投到那个程序自己的线程, 接口**投出去就返回** —— 与真桌面的形状一致
    (SendInput 送出去就返回 / 点 X = 投一条 WM_CLOSE)。所以响应只说明"消息已发出"。
  · "标题变了 / 窗口没了"这类效果断言要自己等一下: 本脚本用 wait_for(); 真桌面上的脚本用
    waitStable() 看画面收敛 —— 两边同一个形状 (这样虚拟桌面里跑通的脚本才能拿到真机复验)。
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# 程序脚本: 与 main.cpp 里的 JS 示范同一套写法 (窗口 + 输入框 + 按钮)
JS_APP = """
const w = new JZWindow(360, 200, '输入测试');
const tip = new JZLabel('点输入框 -> 打字 -> 点按钮', w);
tip.setGeometry(10, 8, 330, 22); tip.show();
const e = new JZLineEdit(w);
e.setGeometry(10, 40, 200, 26); e.show();
const b = new JZPushButton('应用');
b.setParentWidget(w);
b.setGeometry(10, 80, 100, 28); b.show();
b.onClicked = () => { w.setTitle('文本=' + e.text()); };
w.show();
"""


class Fail(Exception):
    pass


class Api:
    def __init__(self, port):
        self.base = "http://127.0.0.1:%d" % port

    def raw(self, method, path, body=None, timeout=30):
        """返回 (status, body_bytes)。"""
        data = None
        if isinstance(body, bytes):
            data = body                      # 手工塞坏 JSON 用 (测 400)
        elif body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    def json(self, method, path, body=None, expect=200):
        status, raw = self.raw(method, path, body)
        if status != expect:
            raise Fail("%s %s -> HTTP %d (期望 %d): %s"
                       % (method, path, status, expect, raw[:200].decode("utf-8", "replace")))
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception as e:
            raise Fail("%s %s -> 响应不是 JSON: %s (%s)" % (method, path, raw[:200], e))

    # ---- 便捷封装 ----
    def health(self):
        return self.json("GET", "/api/health")

    def windows(self):
        return self.json("GET", "/api/windows")

    def screen(self, query=""):
        status, raw = self.raw("GET", "/api/screen" + query)
        if status != 200:
            raise Fail("截图失败: HTTP %d: %s" % (status, raw[:200]))
        return raw

    def click(self, x, y, button="left"):
        return self.json("POST", "/api/input/mouse",
                         {"action": "click", "x": x, "y": y, "button": button})

    def type_text(self, text):
        return self.json("POST", "/api/input/key", {"text": text})

    def window_by_title(self, title):
        for w in self.windows()["windows"]:
            if w["title"] == title:
                return w
        return None


def q(s):
    """查询串里的值必须 percent-encode (中文标题要能当 window= 用)。"""
    return urllib.parse.quote(s, safe="")


def png_size(data):
    """读 PNG 的 IHDR: 不引依赖。"""
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    return (int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big"))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def find_exe(cfg):
    cands = [
        os.path.join(ROOT, "build", "binary", "virtual_device", "bin", cfg, "virtual_device.exe"),
        os.path.join(ROOT, "build", "binary", "virtual_device", cfg, "virtual_device.exe"),
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def wait_for(pred, secs=3.0, step=0.05):
    """等一个"窗口那一侧已经把这条消息处理完"的观察点。

    ⚠ 输入/关窗都是**投递**语义 (同真桌面: SendInput 送出去就返回 / 点 X = 投一条 WM_CLOSE),
      接口响应只说明"消息已发出" ⇒ "标题变了 / 窗口没了"这类断言得自己等一下 ——
      与真桌面上的脚本同一个形状 (那边靠 waitStable 看画面收敛, 这里直接查窗口表更干脆)。
    """
    end = time.time() + secs
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


class Test:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.problems = []

    def check(self, name, cond, extra=""):
        if cond:
            self.passed += 1
            print("  [ ok ] %s" % name)
        else:
            self.failed += 1
            self.problems.append(name + ((" — " + str(extra)) if extra else ""))
            print("  [FAIL] %s%s" % (name, ("  " + str(extra)) if extra else ""))

    def eq(self, name, got, want):
        self.check(name, got == want, "got=%r want=%r" % (got, want))


def main():
    ap = argparse.ArgumentParser(description="虚拟桌面容器 HTTP 接口冒烟测试")
    ap.add_argument("--cfg", default="Debug")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="测完不杀进程")
    args = ap.parse_args()

    exe = args.exe or find_exe(args.cfg)
    if not exe or not os.path.exists(exe):
        print("FAIL: 找不到 virtual_device.exe（先构建 virtual_device 目标, 或 --exe 指定）")
        return 2

    port = args.port or free_port()
    exe_dir = os.path.dirname(exe)
    # 产物全放一个子目录 (exe 目录是部署目录: 里面已经有 jz_js/, 别混进测试的临时文件)
    out_dir = os.path.join(exe_dir, "api_test_out")
    os.makedirs(out_dir, exist_ok=True)
    log_path = os.path.join(out_dir, "server.log")
    log = open(log_path, "wb")
    cmd = [exe, "--headless", "--http-port", str(port), "--desktop", "800x600"]
    print("== 容器: %s" % " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=exe_dir, stdout=log, stderr=subprocess.STDOUT)

    t = Test()
    api = Api(port)
    try:
        # ---- 0) 等它起来 ----
        health = None
        for _ in range(80):
            try:
                health = api.health()
                break
            except Exception:
                if proc.poll() is not None:
                    raise Fail("容器进程退出了 (code=%s), 日志: %s"
                               % (proc.returncode, open(log_path, "rb").read()[-800:]))
                time.sleep(0.25)
        if health is None:
            raise Fail("等 /api/health 超时; 日志: %s" % open(log_path, "rb").read()[-800:])

        print("== 1) 容器状态 ==")
        t.check("health.ok", health.get("ok") is True, health)
        t.eq("桌面逻辑尺寸 800x600",
             (health["desktop"]["w"], health["desktop"]["h"]), (800, 600))
        t.eq("无头下 scale 恒 1 (截图与坐标同一口径)", health["desktop"]["scale"], 1.0)
        t.eq("干净桌面 (无头不预建示范窗口)", health.get("windows"), 0)
        t.eq("程序接口已接上 (宿主登记了 JZVirtualProgramHost)", health.get("host"), "ready")

        print("== 2) 整屏截图 ==")
        shot = api.screen()
        t.eq("整屏 PNG = 桌面逻辑尺寸", png_size(shot), (800, 600))
        with open(os.path.join(out_dir, "desktop.png"), "wb") as f:
            f.write(shot)
        desk2 = api.json("GET", "/api/screen?format=json")
        t.check("format=json 给 dataUrl", desk2.get("dataUrl", "").startswith("data:image/png;base64,"),
                list(desk2.keys()))
        t.eq("json 里也报逻辑尺寸", desk2.get("logical"), [0, 0, 800, 600])

        print("== 3) 新建窗口 (不依赖 JS) ==")
        made = api.json("POST", "/api/windows", {"w": 300, "h": 180, "title": "手建窗口", "x": 40, "y": 30})
        t.check("POST /api/windows 成功", made.get("ok") is True, made)
        w0 = made["window"]
        t.eq("外框几何 = 请求的位置", w0["frame"][:2], [40, 30])
        t.eq("客户区 = 外框 + 装饰 (border 2 / title 22)",
             w0["client"][:2], [40 + 2, 30 + 22])
        t.eq("客户区尺寸 = 请求的客户区", w0["client"][2:], [300, 180])
        t.eq("归属: 宿主自己建的 (无程序)", w0["process"], "")

        print("== 4) 加载程序 (JS 进程) ==")
        prog = api.json("POST", "/api/programs", {"name": "prog1", "script": JS_APP}, expect=200)
        t.check("POST /api/programs 成功", prog.get("ok") is True, prog)
        t.check("响应里带回它建的窗口 (不用轮询)", len(prog.get("windows", [])) == 1, prog.get("windows"))
        js_win = api.window_by_title("输入测试")
        t.check("程序建的窗口在桌面窗口表里", js_win is not None,
                [w["title"] for w in api.windows()["windows"]])
        if js_win:
            t.eq("窗口归属 = 程序名", js_win.get("process"), "prog1")
        progs = api.json("GET", "/api/programs")
        t.eq("GET /api/programs 列出它", [p["name"] for p in progs["programs"]], ["prog1"])

        print("== 5) 客户区截图 ==")
        wshot = api.screen("?window=" + q("输入测试"))
        cw, ch = js_win["client"][2], js_win["client"][3]
        t.eq("窗口截图 = 客户区逻辑尺寸 (无头 scale 1)", png_size(wshot), (cw, ch))
        with open(os.path.join(out_dir, "window.png"), "wb") as f:
            f.write(wshot)

        print("== 6) 鼠标 + 键盘 (点输入框 -> 打字 -> 点按钮) ==")
        cx, cy = js_win["client"][0], js_win["client"][1]
        r = api.click(cx + 100, cy + 52)         # 输入框 (客户区 10,40,200,26) 里
        t.eq("点击落到那个窗口上", r.get("window"), "输入测试")
        r = api.type_text("hello")
        t.check("打字被处理了", r.get("ok") is True and r.get("text") == "hello", r)
        r = api.click(cx + 60, cy + 94)          # 按钮 (10,80,100,28) 里
        t.check("点按钮被处理了", r.get("ok") is True, r)
        # ⚠ 输入是**投递**的 (同 PostMessage/同真桌面 SendInput): 点击/打字投到这个程序自己的线程,
        #   响应只说明"发出去了" ⇒ 效果断言要自己等一下 (见 wait_for)
        t.check("按钮回调读到输入框内容 (标题 = 文本=hello) —— 鼠标/键盘/合成/JS 一条链都通",
                wait_for(lambda: api.window_by_title("文本=hello") is not None),
                [w["title"] for w in api.windows()["windows"]])

        print("== 7) 拖动窗口 (窗口管理器那条路) ==")
        # ⚠ 窗口标题刚被按钮回调改成 "文本=hello" 了 —— 后面的按标题找窗口都用这个新标题
        live = "文本=hello"
        before = api.window_by_title(live)
        fx, fy, fw = before["frame"][0], before["frame"][1], before["frame"][2]
        r = api.json("POST", "/api/input/mouse",
                     {"action": "drag", "x": fx + fw - 60, "y": fy + 11,   # 标题栏空白处
                      "toX": fx + fw - 60 - 70, "toY": fy + 11 + 40, "steps": 6})
        t.check("拖动被处理了", r.get("ok") is True, r)
        moved = api.window_by_title(live)
        t.eq("外框位置真的动了", moved["frame"][:2], [fx - 70, fy + 40])
        # 拖回原位, 免得后面的截图里窗口压着角落
        api.json("POST", "/api/input/mouse",
                 {"action": "drag", "x": moved["frame"][0] + moved["frame"][2] - 60,
                  "y": moved["frame"][1] + 11, "toX": fx + fw - 60, "toY": fy + 11, "steps": 6})

        print("== 8) DPI: scale 只改截图分辨率, 不进坐标 ==")
        s2 = api.json("POST", "/api/desktop", {"scale": 2})
        t.eq("setScale(2) 生效", s2.get("scale"), 2.0)
        t.eq("整屏截图 = 逻辑尺寸 x 2", png_size(api.screen()), (1600, 1200))
        t.eq("窗口截图也 x 2", png_size(api.screen("?window=" + q(live))), (cw * 2, ch * 2))
        still = api.window_by_title(live)
        t.eq("窗口几何仍然是逻辑像素 (坐标与缩放无关)", still["frame"], before["frame"])
        t.eq("命中测试也仍然按逻辑坐标: 点窗口里 -> 是它",
             api.click(still["client"][0] + 100, still["client"][1] + 52).get("window"),
             still["title"])
        api.json("POST", "/api/desktop", {"scale": 1})
        t.eq("改回 1", png_size(api.screen()), (800, 600))

        print("== 9) 程序: 加载 JS 程序(模块) / 停掉 ==")

        # 一个**真程序**: ESM 模块 (有 import/export), 标题里带回"运行根对不对"的结论 ——
        # 只有按模块加载才跑得起来 (`export` 当一段文本跑是语法错误), 也只有模块才有入口目录。
        seg = os.path.basename(out_dir)
        js_path = os.path.join(out_dir, "mod_app.js")
        with open(js_path, "w", encoding="utf-8") as f:
            f.write(
                "import fs from 'fs';\n"
                "const dir = jzApp.applicationDirPath();\n"
                "const ok = (dir.indexOf('" + seg + "') >= 0) && (dir.length > 0);\n"
                "const w = new JZWindow(240, 140,\n"
                "    'mod:' + (fs ? 'fs' : 'nofs') + (ok ? '-dir-ok' : '-dir-bad'));\n"
                "w.show();\n"
                "export const loaded = true;\n")
        prog2 = api.json("POST", "/api/programs", {"name": "prog2", "file": js_path})
        t.check("加载 JS 程序 (file) 成功", prog2.get("ok") is True, prog2)
        t.eq("按模块加载 (不是当文本 eval)", prog2.get("module"), "mod_app.js")
        t.eq("运行根 = 程序所在目录",
             os.path.normcase(os.path.normpath(prog2.get("workDir", ""))),
             os.path.normcase(os.path.normpath(out_dir)))
        want_title = "mod:fs-dir-ok"
        t.eq("程序跑起来了 (import 认了 / 入口目录拿到了)",
             [w["title"] for w in prog2.get("windows", [])], [want_title])
        t.eq("两个程序同时在跑", sorted(p["name"] for p in api.json("GET", "/api/programs")["programs"]),
             ["prog1", "prog2"])

        st = api.json("DELETE", "/api/programs/prog2")
        t.check("停掉 prog2", st.get("ok") is True, st)
        t.check("它的窗口一起收走了",
                not any(w["title"].startswith("mod:") for w in api.windows()["windows"]),
                [w["title"] for w in api.windows()["windows"]])
        t.eq("程序表只剩 prog1", [p["name"] for p in api.json("GET", "/api/programs")["programs"]],
             ["prog1"])

        print("== 9b) 未处理异常 = 那个程序退出 (窗口一起收走) ==")
        # ① 起来就抛: 加载直接失败 (400 + 错误文本), 而不是 ok + 空窗口列表
        status, raw = api.raw("POST", "/api/programs",
                              {"name": "prog_boot", "script": "throw new Error('boot');"})
        t.eq("起来就崩 -> 400", status, 400)
        t.check("错误文本回到调用方", b"boot" in raw, raw[:200])
        t.check("它没留在程序表里",
                "prog_boot" not in [p["name"] for p in api.json("GET", "/api/programs")["programs"]],
                api.json("GET", "/api/programs")["programs"])

        # ② 跑起来再抛: 程序自己退出 -> 从程序表消失 + 窗口一起收走 (容器不退)
        late = ("const w = new JZWindow(220, 120, 'late_boom'); w.show();"
                "setTimeout(() => { throw new Error('late'); }, 400);")
        lp = api.json("POST", "/api/programs", {"name": "prog_late", "script": late})
        t.check("先正常跑起来 (有窗口)", len(lp.get("windows", [])) == 1, lp.get("windows"))
        gone = False
        for _ in range(40):                      # 最多等 2 秒
            time.sleep(0.05)
            if not any(p["name"] == "prog_late" for p in api.json("GET", "/api/programs")["programs"]):
                gone = True
                break
        t.check("异常后程序自己退出 (从程序表消失)", gone, api.json("GET", "/api/programs"))
        t.check("它的窗口一起收走",
                not any(w["title"] == "late_boom" for w in api.windows()["windows"]),
                [w["title"] for w in api.windows()["windows"]])
        t.check("容器自己活着 (没跟着退)", api.json("GET", "/api/health").get("ok") is True)

        print("== 10) 查找 / 枚举窗口 (Windows 风格) ==")
        live_win = api.window_by_title(live)
        handle = live_win["id"]
        t.check("每个窗口都有稳定句柄 id (0x...)", isinstance(handle, str) and handle.startswith("0x"),
                handle)
        ids = [w["id"] for w in api.windows()["windows"]]
        t.eq("句柄互不相同", len(ids), len(set(ids)))
        by_handle = api.json("GET", "/api/windows/" + handle)
        t.eq("按句柄查 = 同一个窗口", by_handle["window"]["title"], live)
        t.eq("按下标查 = 同一个窗口",
             api.json("GET", "/api/windows/%d" % live_win["index"])["window"]["title"], live)
        # `#` 是 URL 片段分隔符, 路径里用不了 —— 只在 query 里写 %23 (query 值会做 URL 解码)
        t.eq("query 里 %23 下标也认 (解码后 = #下标)",
             png_size(api.screen("?window=%23" + str(live_win["index"]))),
             (live_win["client"][2], live_win["client"][3]))
        t.eq("按标题片段查 = 同一个窗口",
             api.json("GET", "/api/windows/" + q("文本"))["window"]["title"], live)
        t.eq("?title= 过滤", [w["title"] for w in api.json(
            "GET", "/api/windows?title=" + q("手建"))["windows"]], ["手建窗口"])
        t.eq("?process= 过滤 (某个程序开了哪些窗口)",
             [w["title"] for w in api.json("GET", "/api/windows?process=prog1")["windows"]], [live])
        t.eq("?visible=1 过滤", api.json("GET", "/api/windows?visible=1")["count"],
             api.windows()["count"])
        # WindowFromPoint: 客户区里一点 -> 是它, 且顺带给出该点在客户区里的坐标
        pt = api.json("GET", "/api/windows?at=%d,%d" % (live_win["client"][0] + 100,
                                                        live_win["client"][1] + 52))
        t.eq("at=x,y 命中客户区 -> 是那个窗口", [w["title"] for w in pt["windows"]], [live])
        t.eq("at= 顺带给出客户区坐标", pt["clientAt"], [100, 52])
        t.eq("at= 落到任务栏上 -> 没有窗口 (chrome 不参与命中)",
             api.json("GET", "/api/windows?at=20,595")["count"], 0)

        print("== 11) 直接给窗口发鼠标/键盘 (不激活它) ==")
        # 先让"手建窗口"成为活动窗口, 之后全程不对它做任何事 —— 操作全靠直投给 prog1 的窗口
        api.json("POST", "/api/windows/" + q("手建窗口") + "/state", {"action": "activate"})
        t.eq("准备: 活动窗口是手建窗口", api.windows()["windows"][api.windows()["active"]]["title"],
             "手建窗口")
        c0 = live_win["client"]
        r = api.json("POST", "/api/windows/" + handle + "/input",
                     {"type": "mouse", "action": "click", "x": 100, "y": 52})   # 客户区坐标
        t.eq("直投点击: 目标 = 那个窗口, 坐标系 = client", (r["window"], r["coords"]), (live, "client"))
        r = api.json("POST", "/api/windows/" + handle + "/input", {"type": "key", "text": "world"})
        t.check("直投打字被处理", r.get("ok") is True and r.get("handled") is True, r)
        r = api.json("POST", "/api/windows/" + handle + "/input",
                     {"type": "mouse", "action": "click", "x": 60, "y": 94})     # 按钮
        t.check("直投点击按钮", r.get("ok") is True, r)
        # 输入框里上一轮留下的 "hello" 还在, 直投的 "world" 接在后面 ⇒ 是插到光标处 (没覆盖)
        live = "文本=helloworld"
        t.check("后台窗口也能被操作 (标题 = 文本=helloworld)",
                wait_for(lambda: api.window_by_title(live) is not None),   # 同上: 投递, 等它处理
                [w["title"] for w in api.windows()["windows"]])
        t.eq("它全程没被激活", api.windows()["windows"][api.windows()["active"]]["title"],
             "手建窗口")
        # coords=screen 时用桌面坐标, 换算结果回在 client 里
        live_win = api.window_by_title(live)
        sx, sy = live_win["client"][0] + 10, live_win["client"][1] + 10
        r = api.json("POST", "/api/windows/" + handle + "/input",
                     {"type": "mouse", "action": "move", "coords": "screen", "x": sx, "y": sy})
        t.eq("coords=screen: 内部换算成客户区", (r["coords"], r["client"]), ("screen", [10, 10]))

        print("== 12) 窗口状态 / 位置 / 标题 (SetWindowPos 那一组) ==")
        # 装饰差从窗口自己读出来 (4 = 2*border, 24 = title 22 + border 2), 免得测试里抄常量
        dw = live_win["frame"][2] - live_win["client"][2]
        dh = live_win["frame"][3] - live_win["client"][3]
        st = api.json("POST", "/api/windows/" + handle + "/state",
                      {"w": 400, "h": 300, "x": 10, "y": 10, "title": "改过名了"})
        w2 = st["window"]
        t.eq("改客户区尺寸 (w/h 是客户区口径)", w2["client"][2:], [400, 300])
        t.eq("外框跟着长 (客户区 + 装饰)", w2["frame"][2:], [400 + dw, 300 + dh])
        t.eq("移动 = 设外框位置", w2["frame"][:2], [10, 10])
        t.eq("改名", w2["title"], "改过名了")
        handle2 = w2["id"]
        t.eq("句柄不随改名/移动变", handle2, handle)
        st = api.json("POST", "/api/windows/" + handle2 + "/state", {"action": "minimize"})
        t.eq("最小化", st["window"]["minimized"], True)
        st = api.json("POST", "/api/windows/" + handle2 + "/state", {"action": "maximize"})
        m = st["window"]
        t.check("最大化铺到工作区 (不盖任务栏)",
                m["maximized"] is True and m["frame"] == [0, 0, 800, 600 - 28], m["frame"])
        st = api.json("POST", "/api/windows/" + handle2 + "/state", {"action": "restore"})
        t.eq("还原回到原来的位置尺寸", st["window"]["frame"], w2["frame"])
        # 关掉一个窗口: 两种写法 (DELETE 与 action=close) 都要能用
        throwaway = api.json("POST", "/api/windows", {"w": 120, "h": 80, "title": "待关窗口"})["window"]
        st = api.json("POST", "/api/windows/" + throwaway["id"] + "/state", {"action": "close"})
        t.check("action=close 关窗", st.get("ok") is True, st)
        t.check("关掉后查不到了",
                api.json("GET", "/api/windows/" + throwaway["id"], expect=404)["ok"] is False)
        t.eq("关掉后从窗口表消失", api.window_by_title("待关窗口"), None)

        print("== 12b) 关掉**程序**的窗口 (点标题栏关闭按钮那条路) ==")
        # 程序窗口的客户区住在**那个程序的线程**里: 关它必须回到它的线程做 (桌面把"关窗"投过去)。
        # 这一条曾经把容器直接打崩 (主线程跨线程 delete 客户区 -> JZObject 的线程亲和断言),
        # 所以这里连"点关闭按钮"与"关掉窗口后再停程序"两条都验一遍。
        pc = api.json("POST", "/api/programs",
                      {"name": "prog_close",
                       "script": "const w = new JZWindow(260, 140, 'pc-win'); w.show();"})
        t.check("加载一个待关的程序", pc.get("ok") is True, pc)
        pwin = api.window_by_title("pc-win")
        t.check("它的窗口在窗口表里", pwin is not None,
                [w["title"] for w in api.windows()["windows"]])
        # 关闭按钮中心: 外框右上角往里 (右边框 2 + 4 起算, 按钮 12x12, 标题栏高 22 取半)
        bx = pwin["frame"][0] + pwin["frame"][2] - 12
        by = pwin["frame"][1] + 11
        r = api.click(bx, by)
        t.check("点程序窗口的关闭按钮被处理", r.get("ok") is True, r)
        # 关窗 = 投一条 close 消息 (同真桌面点 X = WM_CLOSE), 响应时窗口可能还在表里一下 ⇒ 等它落地
        t.check("程序窗口关了 (从窗口表消失)",
                wait_for(lambda: api.window_by_title("pc-win") is None),
                [w["title"] for w in api.windows()["windows"]])
        t.check("关窗口 ≠ 停程序 (它还在程序表里)",
                "prog_close" in [p["name"] for p in api.json("GET", "/api/programs")["programs"]],
                api.json("GET", "/api/programs"))
        t.check("别的窗口不受影响", api.window_by_title("改过名了") is not None,
                [w["title"] for w in api.windows()["windows"]])
        st = api.json("DELETE", "/api/programs/prog_close")
        t.check("窗口已被关掉的程序也停得掉 (不再踩它缓存的野窗口指针)", st.get("ok") is True, st)

        pc2 = api.json("POST", "/api/programs",
                       {"name": "prog_close2",
                        "script": "const w = new JZWindow(200, 120, 'pc2-win'); w.show();"})
        t.check("再加载一个待关的程序", pc2.get("ok") is True, pc2)
        st = api.json("DELETE", "/api/windows/" + q("pc2-win"))
        t.check("DELETE /api/windows/<程序窗口> 也关得掉", st.get("ok") is True, st)
        t.check("它也从窗口表消失 (等 close 消息被那个程序处理完)",
                wait_for(lambda: api.window_by_title("pc2-win") is None),
                [w["title"] for w in api.windows()["windows"]])
        t.check("两条路走完容器仍然活着 (关程序窗口不把容器带走)",
                api.health().get("ok") is True, api.health())
        api.json("DELETE", "/api/programs/prog_close2")

        print("== 13) 错误路径 ==")
        status, raw = api.raw("GET", "/api/nope")
        t.eq("未知路由 -> 404", status, 404)
        status, raw = api.raw("POST", "/api/input/mouse", None)   # 空体 = 没有 action
        t.eq("缺参数 -> 400", status, 400)
        status, raw = api.raw("POST", "/api/input/key", b"{not json")  # 手工塞坏 JSON
        t.check("坏 JSON -> 400", status == 400, status)
        err = api.json("GET", "/api/screen?window=" + q("不存在的窗口"), expect=404)
        t.check("找不到窗口 -> 404 + 中文原因没乱码",
                ("没有这个窗口" in err.get("error", "")), err)
        status, raw = api.raw("GET", "/api/windows/0xdeadbeef")
        t.eq("查不存在的句柄 -> 404", status, 404)
        status, raw = api.raw("GET", "/api/windows?at=abc")
        t.eq("at 写错 -> 400", status, 400)
        status, raw = api.raw("POST", "/api/windows/" + handle2 + "/state", {"action": "爆炸"})
        t.eq("不认识的 action -> 400", status, 400)
        status, raw = api.raw("POST", "/api/windows/" + handle2 + "/input", {"type": "键盘"})
        t.eq("type 只能 mouse/key -> 400", status, 400)
        status, raw = api.raw("POST", "/api/windows/" + handle2 + "/state", {"w": 0, "h": 0})
        t.eq("w/h <= 0 -> 400", status, 400)
        api.json("DELETE", "/api/programs/prog1")   # 收尾: 停掉程序

        print("== 14) 命令行直接加载程序 (--program, 不用 HTTP) ==")
        cli_port = free_port()
        cli_log = os.path.join(out_dir, "cli_server.log")
        with open(cli_log, "wb") as lf:
            cli = subprocess.Popen([exe, "--headless", "--http-port", str(cli_port),
                                    "--program", js_path, "--program-name", "cli-app"],
                                   cwd=exe_dir, stdout=lf, stderr=subprocess.STDOUT)
        try:
            cli_api = Api(cli_port)
            got = None
            for _ in range(80):
                try:
                    got = cli_api.windows()
                    if got["count"] > 0:
                        break
                except Exception:
                    if cli.poll() is not None:
                        raise Fail("--program 那个容器退出了: %s"
                                   % open(cli_log, "rb").read()[-400:])
                time.sleep(0.25)
            t.check("--program: 开机就加载了程序并出窗口", got is not None and got["count"] == 1, got)
            if got and got["count"] == 1:
                t.eq("--program: 程序名与标题都对",
                     (got["windows"][0]["process"], got["windows"][0]["title"]),
                     ("cli-app", want_title))
            log_text = open(cli_log, "rb").read().decode("utf-8", "replace")
            t.check("--program: 启动日志里有加载记录", "已加载" in log_text, log_text[-200:])
        finally:
            cli.terminate()
            try:
                cli.wait(timeout=10)
            except Exception:
                cli.kill()

        print("== 15) 程序输出 (/api/logs: 按行取 + offset/size) ==")
        # 本节自己加载程序来打日志: 先 5 行, 再 1200 行验证"只留最近 1000 条"的滚动
        api.json("POST", "/api/programs",
                 {"name": "logger",
                  "script": 'for (var i = 0; i < 5; i++) console.log("line-" + i);'})
        time.sleep(1.2)
        a = api.json("GET", "/api/logs")
        t.check("不带参数 = 读全部 (count == total)", a["count"] == a["total"] and a["total"] >= 5, a)
        t.check("行文本带程序名前缀, 最后一行是本程序打的",
                all(s.startswith("[vproc: ") for s in a["lines"])
                and a["lines"][-1] == "[vproc: logger] line-4", a["lines"][:3])
        part = api.json("GET", "/api/logs?offset=%d&size=1" % (a["total"] - 1))
        t.eq("offset/size 精确取一行", part["lines"], ["[vproc: logger] line-4"])
        t.eq("size=-1 = 一直读到末尾", api.json("GET", "/api/logs?size=-1")["count"], a["total"])
        t.eq("offset 给得太晚 -> 空", api.json("GET", "/api/logs?offset=999999")["lines"], [])
        api.json("POST", "/api/programs",
                 {"name": "bulk",
                  "script": 'for (var i = 0; i < 1200; i++) console.log("bulk-" + i);'})
        time.sleep(3.0)
        b = api.json("GET", "/api/logs")
        t.eq("超 1000 条只留最近 1000", b["count"], 1000)
        t.eq("total 是累计行数 (含已挤掉的)", b["total"], a["total"] + 1200)
        t.eq("first = 缓冲里第一行的绝对行号", b["first"], b["total"] - 1000)
        t.eq("first 那行就是最老还留着的那行",
             b["lines"][0], "[vproc: bulk] bulk-%d" % (b["first"] - a["total"]))
        t.eq("记住 total 增量拉 (此刻还没新行)",
             api.json("GET", "/api/logs?offset=%d" % b["total"])["lines"], [])
        api.json("DELETE", "/api/programs/logger")
        api.json("DELETE", "/api/programs/bulk")

    except Fail as e:
        print("FAIL: %s" % e)
        t.failed += 1
        t.problems.append(str(e))
    finally:
        if args.keep:
            print("== 容器还在跑 (pid=%d, port=%d; --keep) ==" % (proc.pid, port))
        else:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
            log.close()

    print("\n== 结果: %d 通过 / %d 失败 ==" % (t.passed, t.failed))
    for p in t.problems:
        print("  - %s" % p)
    print("== %s ==" % ("PASS" if t.failed == 0 else "FAIL"))
    return 0 if t.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
