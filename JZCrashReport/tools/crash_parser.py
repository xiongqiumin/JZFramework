import re
import subprocess
import argparse
import os
import sys
import shutil
import glob

# ESP32 裸地址格式(来自 JZCrashReportManager::takePendingReport): 每行一个 0xXXXXXXXX
ESP_ADDR_PATTERN = re.compile(r"^\s*0x([0-9a-fA-F]+)\s*$")
# Linux backtrace_symbols 格式: binary(+0xoffset) [0xaddr]
LINUX_PATTERN = re.compile(r"([^\s]+)\(\+0x([0-9a-fA-F]+)\)\s+\[0x([0-9a-fA-F]+)\]")
# 报告头部 Platform 行(由 JZCrashReportManager::makeReport 生成)
PLATFORM_PATTERN = re.compile(r"^\s*Platform:\s*([A-Za-z0-9_]+)\s*$", re.IGNORECASE)


def addr2line_batch(addresses, addr2line_path, exe_path):
    """ 一次性解析多个地址, 返回与 addresses 等长的 (函数名, 位置) 列表。
        addr2line 支持一次传入多个地址, 按序输出 func/loc 两行一组, 比逐行调用快很多。 """
    results = [("??", "??")] * len(addresses)
    if not addresses:
        return results
    try:
        cmd = [addr2line_path, "-f", "-C", "-e", exe_path] + addresses
        output = subprocess.check_output(cmd, encoding="utf-8", stderr=subprocess.STDOUT)
        lines = [ln for ln in output.strip().split('\n') if ln != ""]
        for i in range(0, min(len(addresses), len(lines) // 2)):
            results[i] = (lines[2 * i], lines[2 * i + 1])
    except Exception:
        pass
    return results


def detect_platform(crash_report):
    """ 从报告文本中读取 Platform 行; 必须存在, 不支持无平台信息的旧报告 """
    for line in crash_report.splitlines():
        m = PLATFORM_PATTERN.match(line)
        if m:
            return m.group(1).lower()
    return None


def resolve_addr2line(platform, explicit):
    """ 确定 addr2line 路径: 显式参数优先, 否则按平台从环境推导 """
    if explicit:
        return explicit
    if platform == "esp32":
        # 1) IDF 的 export 会把工具链加入 PATH, 直接 which 即可
        for name in ("xtensa-esp32-elf-addr2line", "riscv32-esp-elf-addr2line"):
            p = shutil.which(name)
            if p:
                return p
        # 2) 回退: 通过 IDF_PATH / 默认安装目录推导
        idf = os.environ.get("IDF_PATH")
        search_dirs = []
        if idf:
            search_dirs.append(os.path.join(idf, "..", "esp-idf-tools", "tools", "xtensa-esp32-elf"))
        search_dirs.append(os.path.expanduser(os.path.join("~", ".espressif", "tools", "xtensa-esp32-elf")))
        for base in search_dirs:
            matches = sorted(
                glob.glob(os.path.join(base, "*", "xtensa-esp32-elf", "bin", "xtensa-esp32-elf-addr2line")),
                reverse=True)
            if matches:
                return matches[0]
        # 3) 最终回退(若不在 PATH, subprocess 会给出明确报错)
        return "xtensa-esp32-elf-addr2line"
    # Linux/Windows 默认系统 addr2line
    return "/usr/bin/addr2line"


def _fmt_frame(frame_index, address, func_name, location, binary=None, offset=None):
    if location != "??" and ":" in location:
        file_line = re.sub(r'\s*\(discriminator\s+\d+\)', '', location)
        return f"#{frame_index} {address} in {func_name} {file_line}"
    if binary and offset:
        return f"#{frame_index} {address} in {func_name} ({binary}+{offset})"
    return f"#{frame_index} {address} in {func_name} (??)"


def parse_esp32(crash_report, addr2line_path, exe_path):
    # 收集所有裸地址, 一次性批量解析
    addrs = []
    for line in crash_report.splitlines():
        m = ESP_ADDR_PATTERN.match(line)
        if m:
            addrs.append("0x" + m.group(1))
    syms = addr2line_batch(addrs, addr2line_path, exe_path)

    stack_lines = []
    for i, line in enumerate(crash_report.splitlines()):
        m = ESP_ADDR_PATTERN.match(line)
        if not m:
            continue
        address = "0x" + m.group(1)
        func_name, location = syms[len(stack_lines)]  # 第 i 个匹配对应第 i 组结果
        stack_lines.append(_fmt_frame(len(stack_lines), address, func_name, location))
    return "\n".join(stack_lines)


def parse_linux(crash_report, addr2line_path, exe_path):
    exe_basename = os.path.basename(exe_path)
    # 先扫描, 仅把属于目标程序的帧地址收集起来批量解析
    raw = []          # (binary, offset, address, 目标帧在 target_addrs 中的下标 或 None)
    target_addrs = []
    for line in crash_report.splitlines():
        m = LINUX_PATTERN.search(line)
        if not m:
            continue
        binary = m.group(1)
        offset = "0x" + m.group(2)
        address = "0x" + m.group(3)
        binary_basename = os.path.basename(binary)
        if exe_basename in binary_basename:
            raw.append((binary, offset, address, len(target_addrs)))
            target_addrs.append(offset)
        else:
            raw.append((binary, offset, address, None))
    syms = addr2line_batch(target_addrs, addr2line_path, exe_path)

    stack_lines = []
    for (binary, offset, address, tpos) in raw:
        if tpos is not None:
            func_name, location = syms[tpos]
            stack_lines.append(_fmt_frame(len(stack_lines), address, func_name, location, binary, offset))
        else:
            stack_lines.append(f"#{len(stack_lines)} {address} in ({binary}+{offset})")
    return "\n".join(stack_lines)


def parse_crash_report(crash_report, addr2line_path, exe_path, platform):
    if platform == "esp32":
        return parse_esp32(crash_report, addr2line_path, exe_path)
    return parse_linux(crash_report, addr2line_path, exe_path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="崩溃日志解析工具(支持 Linux / ESP32)")

    parser.add_argument("log", help="崩溃报告文件路径(由 JZCrashReportManager::makeReport 生成)")

    parser.add_argument("--addr2line", default=None,
                        help="addr2line 工具路径。不指定时按 Platform 自动推导: "
                             "ESP32 取 IDF 工具链(需已 source IDF 的 export 或设置 IDF_PATH), "
                             "Linux/Windows 取系统 /usr/bin/addr2line")

    parser.add_argument("--bin", required=True,
                        help="带调试信息的可执行文件/ELF 路径(ESP32 即 build/app.elf)")

    args = parser.parse_args()

    with open(args.log, 'r', encoding='utf-8') as f:
        content = f.read()

    platform = detect_platform(content)
    if platform is None:
        print("错误: 崩溃报告中未找到 'Platform:' 行, 无法确定解析方式。", file=sys.stderr)
        print("       仅支持由 JZCrashReportManager::makeReport 生成的新格式报告。", file=sys.stderr)
        sys.exit(1)
    addr2line_path = resolve_addr2line(platform, args.addr2line)

    print(f"[platform] {platform}")
    print(f"[addr2line] {addr2line_path}")
    print("-------------- symbolized backtrace --------------")

    parsed_result = parse_crash_report(content, addr2line_path, args.bin, platform)
    print(parsed_result)
