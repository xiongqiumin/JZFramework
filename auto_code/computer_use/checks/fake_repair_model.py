"""fake_repair_model.py —— **只为验证管道**的假"修复模型"（OpenAI 兼容 chat/completions）

为什么需要它：真模型端点(内网 GPU 机)不是总在，
而"界面不匹配 → 升级模型 → 拿回当前路径 → 落 override"这条链**不该依赖它的可用性**。
真模型要验的时候，用 run_repair_live.py --url <真端点>（或直接改 runs/current/repair.json）。

它做的事**只有一件事**，而且是"小模型也能做"的那件：
    从证据包里的"当前布局树"里，挑一个与失败路径**最像**的路径返回。
（相似度 = 末段去掉非字母数字、小写后，一个是否为另一个的前缀；不够像就返回空 ⇒ 不采纳。）
即：它不做语义推理、不认识业务，只证明"证据包格式 + 解析 + 校验 + override + 重试"是通的。

用法: python fake_repair_model.py [port]     # 默认 8731
"""
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8731


def norm(s: str) -> str:
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def all_paths(tree: dict):
    """递归收集布局树里的所有路径（与 layout.ts 的规则一致：id || role，重名补 #n）。"""
    out = []

    def walk(n, path):
        if not isinstance(n, dict):
            return
        out.append(path)
        used = {}
        for c in (n.get("children") or []):
            seg = norm(str(c.get("id") or c.get("role") or "node"))
            seg = str(c.get("id") or c.get("role") or "node")
            used[seg] = used.get(seg, 0) + 1
            if used[seg] > 1:
                seg = f"{seg}#{used[seg]}"
            walk(c, f"{path}/{seg}")

    if tree:
        walk(tree, "/" + str(tree.get("id") or tree.get("role") or "root"))
    return out


def pick(text: str):
    """从证据包里挑出"最像失败路径"的那条当前路径。"""
    m = re.search(r"^# 目标路径:\s*(\S+)", text, re.M)
    want = m.group(1) if m else ""
    # 证据包末尾是 "# 当前布局树:\n<JSON>"
    i = text.find("# 当前布局树:")
    tree = None
    if i >= 0:
        j = text.find("{", i)
        if j >= 0:
            try:
                tree, _ = json.JSONDecoder().raw_decode(text[j:])
            except Exception:
                tree = None
    paths = all_paths(tree) if tree else []
    want_seg = norm(want.split("/")[-1])
    best, best_score = "", 0.0
    for p in paths:
        seg = norm(p.split("/")[-1])
        if not seg or not want_seg:
            continue
        score = 0.0
        if seg == want_seg:
            score = 1.0
        elif seg.startswith(want_seg) or want_seg.startswith(seg):
            score = min(len(seg), len(want_seg)) / max(len(seg), len(want_seg))
        if score > best_score:
            best, best_score = p, score
    return (best if best_score >= 0.5 else ""), want, paths


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        msgs = body.get("messages") or []
        text = ""
        for m in msgs:
            c = m.get("content")
            if isinstance(c, str):
                text += c + "\n"
            elif isinstance(c, list):
                for part in c:
                    if isinstance(part, dict) and part.get("type") == "text":
                        text += str(part.get("text") or "") + "\n"
        new_path, want, paths = pick(text)
        ans = {
            "kind": "path-drift" if new_path else "page-miss",
            "action": "ask-model",
            "newPath": new_path,
            "reason": f"在 {len(paths)} 条路径里选最像 {want} 的一条（假模型：只做前缀相似度）",
        }
        content = json.dumps(ans, ensure_ascii=False)
        out = {"choices": [{"message": {"role": "assistant", "content": content}, "finish_reason": "stop"}]}
        raw = json.dumps(out, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)
        sys.stderr.write(f"[fake-repair] {want} -> {new_path or '(空)'}\n")
        sys.stderr.flush()


if __name__ == "__main__":
    srv = HTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write(f"[fake-repair] listening on http://127.0.0.1:{PORT}/v1/chat/completions\n")
    sys.stderr.flush()
    srv.serve_forever()
