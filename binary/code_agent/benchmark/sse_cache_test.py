# 探测 SSE 是否返回 cache 输入/输出 token
# 用法: python sse_cache_test.py
# 关键: stream=true + stream_options.include_usage=true，usage 只在结尾那个 choices=[] 的包里。
# 连发 3 次相同请求: 若服务端支持前缀缓存，第 2/3 次的 prompt_tokens_details.cached_tokens 应 > 0。
import json
import urllib.request

URL = "http://172.17.160.147:8080/v1/chat/completions"
MODEL = "my_model"
TOKEN = ""  # modelToken 为空


def chat_once(messages):
    body = {
        "model": MODEL,
        "messages": messages,
        "stream": True,
        "stream_options": {"include_usage": True},
        "max_tokens": 256,
    }
    req = urllib.request.Request(URL, data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + TOKEN)

    last_usage = None
    text = []
    with urllib.request.urlopen(req, timeout=120) as resp:
        for raw in resp:
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("data:"):
                continue
            payload = line[len("data:"):].strip()
            if payload == "[DONE]":
                continue
            try:
                obj = json.loads(payload)
            except Exception:
                continue
            for ch in obj.get("choices", []):
                d = ch.get("delta", {})
                c = d.get("content")
                if isinstance(c, str):
                    text.append(c)
            if obj.get("usage"):
                last_usage = obj["usage"]
    return last_usage, "".join(text)


if __name__ == "__main__":
    # 拉长 system 前缀，便于触发前缀缓存（多数服务端对短前缀不缓存）
    sys_prompt = "你是一个严谨的代码助手。" + ("请用简体中文、条理清晰、尽量少废话地回答。" * 40)
    messages = [
        {"role": "system", "content": sys_prompt},
        {"role": "user", "content": "用一句话介绍你自己，并写一个 hello world 的 Python 函数。"},
    ]

    for i in range(3):
        u, t = chat_once(messages)
        print(f"=== 请求 {i + 1} ===")
        if not u:
            print("  (未返回 usage —— 服务端可能忽略了 include_usage)")
        else:
            print("  prompt_tokens     =", u.get("prompt_tokens"))
            print("  completion_tokens =", u.get("completion_tokens"))
            print("  total_tokens      =", u.get("total_tokens"))
            print("  prompt_tokens_details     =", json.dumps(u.get("prompt_tokens_details"), ensure_ascii=False))
            print("  completion_tokens_details =", json.dumps(u.get("completion_tokens_details"), ensure_ascii=False))
        print("  reply:", t[:80].replace("\n", " "))
        print()
