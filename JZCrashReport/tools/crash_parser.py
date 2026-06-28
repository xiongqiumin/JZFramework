import re
import subprocess
import argparse
import os

def addr2line(offset, addr2line_path, exe_path):
    """ 解析偏移地址，返回函数名和位置信息 """
    try:
        output = subprocess.check_output(
            [addr2line_path, "-f", "-C", "-e", exe_path, offset],
            encoding="utf-8",
            stderr=subprocess.STDOUT
        )
        lines = output.strip().split('\n')
        if len(lines) >= 2:
            func_name = lines[0]
            location = lines[1]
            return func_name, location
        return "??", "??"
    except Exception:
        return "??", "??"

def parse_crash_report(crash_report, addr2line_path, exe_path):
    lines = crash_report.splitlines()
    stack_lines = []
    pattern = re.compile(r"([^\s]+)\(\+0x([0-9a-fA-F]+)\)\s+\[0x([0-9a-fA-F]+)\]")
    frame_index = 0
    
    exe_basename = os.path.basename(exe_path)

    for line in lines:
        match = pattern.search(line)
        if match:
            binary = match.group(1)
            offset = "0x" + match.group(2)
            address = "0x" + match.group(3)
            
            binary_basename = os.path.basename(binary)
            
            if exe_basename in binary_basename:
                func_name, location = addr2line(offset, addr2line_path, exe_path)
                
                if location != "??" and ":" in location:
                    file_line = re.sub(r'\s*\(discriminator\s+\d+\)', '', location)
                    stack_lines.append(f"#{frame_index} {address} in {func_name} {file_line}")
                else:
                    stack_lines.append(f"#{frame_index} {address} in {func_name} ({binary}+{offset})")
            else:
                stack_lines.append(f"#{frame_index} {address} in ({binary}+{offset})")
            
            frame_index += 1

    return "\n".join(stack_lines)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="崩溃日志解析工具")

    parser.add_argument("log", help="崩溃报告文件路径")

    parser.add_argument("--addr2line", default="/usr/bin/addr2line", help="addr2line 工具路径")

    parser.add_argument("--bin", required=True, help="带调试信息的可执行文件路径")

    args = parser.parse_args()

    with open(args.log, 'r', encoding='utf-8') as f:
        content = f.read()

    parsed_result = parse_crash_report(content, args.addr2line, args.bin)

    print(parsed_result)
