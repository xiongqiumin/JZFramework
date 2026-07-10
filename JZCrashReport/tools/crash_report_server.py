import os
from datetime import datetime

from flask import Flask, request

app = Flask(__name__)

# 保存根目录, 可用环境变量 CRASH_REPORT_DIR 覆盖, 默认 ./crash_reports
SAVE_ROOT = os.environ.get("CRASH_REPORT_DIR", "crash_reports")


def _platform_of(text):
    """ 从报告文本里读取 Platform 行, 用于文件名标记 """
    for line in text.splitlines():
        if line.lower().startswith("platform:"):
            return line.split(":", 1)[1].strip()
    return ""


@app.route("/", methods=["GET"])
def index():
    return "JZCrashReport server. POST a crash report to /report", 200


@app.route("/report", methods=["POST"])
def receive_report():
    # 支持 text/plain(原始 makeReport 文本) 或任意二进制内容
    data = request.get_data()
    try:
        text = data.decode("utf-8", errors="replace")
    except Exception:
        text = ""

    now = datetime.now()
    date_dir = now.strftime("%Y-%m-%d")      # 按日期分目录
    save_dir = os.path.join(SAVE_ROOT, date_dir)
    os.makedirs(save_dir, exist_ok=True)

    ts = now.strftime("%H%M%S") + f"_{now.microsecond:06d}"
    platform = _platform_of(text)
    fname = f"{ts}_{platform}.txt" if platform else f"{ts}.txt"
    path = os.path.join(save_dir, fname)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)

    return {"status": "ok", "saved": os.path.relpath(path)}, 200


if __name__ == "__main__":
    port = int(os.environ.get("CRASH_REPORT_PORT", "5000"))
    host = os.environ.get("CRASH_REPORT_HOST", "0.0.0.0")
    # 关闭多线程/debug 即可, 单进程顺序写文件足够
    app.run(host=host, port=port)
