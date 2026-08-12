#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
esp32.py - JZFramework ESP32 发布脚本

功能:
  1. 将各模块(JZAgent JZAudio JZBle JZCore JZCoroutine JZFrp JZGpio
     JZHttpServer JZModbus JZSerialPort JZWifi)顶层 *.h 头文件拷贝到
     output/include/<模块名>/。
  2. 根据平台把 input 目录下各平台编译产物 JZFramework.a 拷贝到
     output/lib/<平台>/ (约定: input/<平台>/JZFramework.a)。
  3. 在 output/ 下自动生成 ESP-IDF 组件配置文件:
     - CMakeLists.txt   使用 idf_component_register 注册头文件路径
     - idf_component.yml 组件清单(声明依赖/版本等元数据)

ESP-IDF 组件文件说明:
  - CMakeLists.txt: 组件构建文件, 通过 idf_component_register() 声明
        SRCS(源文件)、INCLUDE_DIRS(头文件搜索路径)、REQUIRES(依赖组件)等。
  - idf_component.yml: 组件清单(manifest), 供 IDF 组件管理器使用, 描述
        组件名、版本、依赖(dependencies)等元数据, 可被其他项目通过
        组件管理器引入。

用法:
  python esp32.py --input <编译产物目录> [--output <输出目录>]
  (根目录固定为脚本上两级的 JZFramework, 不通过参数传入)
"""

import argparse
import os
import re
import shutil
import sys

# 需要发布的模块列表(顺序对应组件 REQUIRES 依赖顺序)
MODULES = [
    "JZCore",
    "JZAgent",
    "JZAudio",
    "JZBle",
    "JZCoroutine",
    "JZFrp",
    "JZGpio",
    "JZHttpServer",
    "JZModbus",
    "JZSerialPort",
    "JZWifi",
]


def copy_module_headers(root, output, module):
    """将模块顶层 *.h 拷贝到 output/include/<module>/ , 返回拷贝文件数。"""
    src_dir = os.path.join(root, module)
    if not os.path.isdir(src_dir):
        print("[warn] 模块目录不存在, 跳过: %s" % src_dir)
        return 0

    dst_dir = os.path.join(output, "include", module)
    os.makedirs(dst_dir, exist_ok=True)

    count = 0
    for name in sorted(os.listdir(src_dir)):
        if not name.endswith(".h"):
            continue
        src = os.path.join(src_dir, name)
        if not os.path.isfile(src):
            continue
        dst = os.path.join(dst_dir, name)
        shutil.copy2(src, dst)
        count += 1

    print("[ok] %-12s -> include/%s/  (%d 个头文件)" % (module, module, count))
    return count


def gen_cmakelists(output, modules):
    """生成 output/CMakeLists.txt (按 IDF 官方预编译库方式)。

    ESP-IDF 官方对预编译静态库的做法:
      - 头文件放 include/
      - 各平台库放 lib/<IDF_TARGET>/<name>.a  (如 lib/esp32s3/JZFramework.a)
      - 在组件 CMakeLists.txt 中用 add_prebuilt_library() 注册, 路径按
        ${IDF_TARGET} 拼接, 再用 idf_component_register(LIBS ...) 链接。
    这里用 if(EXISTS ...) 包裹, 仅当对应平台库存在时才链接, 否则退化为
    仅头文件组件。

    注意: REQUIRES 只声明【真实存在的 IDF 组件】。JZCore/JZAgent 等是
    本组件内的子目录(已通过 INCLUDE_DIRS 暴露), 不是独立组件, 绝不能写
    进 REQUIRES, 否则 IDF 依赖搜索会报 "component not found"。
    """
    include_dirs = " ".join('"include/%s"' % m for m in modules)
    # 仅声明 JZFramework 实际依赖的 IDF 公共组件(真实存在, 可被搜索到)
    requires = "freertos driver esp_timer nvs_flash esp-tls"

    content = """# JZFramework ESP-IDF 组件构建文件
# 由 tools/release/esp32.py 自动生成, 请勿手工修改
#
# 按 ESP-IDF 官方预编译库约定:
#   - include/         : 对外头文件
#   - lib/<IDF_TARGET>/: 各平台预编译静态库 (如 lib/esp32s3/JZFramework.a)
#   - add_prebuilt_library(): 按 ${IDF_TARGET} 拼接路径注册预编译库
#   - idf_component_register(LIBS): 将预编译库链接进本组件

idf_component_register(
    INCLUDE_DIRS __INCLUDE_DIRS__
    REQUIRES __REQUIRES__
)

# 按平台引入预编译库 (仅当对应平台库存在时)
set(JZFRAMEWORK_LIB "lib/${IDF_TARGET}/JZFramework.a")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/${JZFRAMEWORK_LIB}")
    add_prebuilt_library(jzframework_prebuilt "${JZFRAMEWORK_LIB}"
        REQUIRES __REQUIRES__)
    target_link_libraries(${COMPONENT_TARGET} INTERFACE jzframework_prebuilt)
    message(STATUS "JZFramework: 链接预编译库 ${JZFRAMEWORK_LIB}")
else()
    message(STATUS "JZFramework: 未找到 ${JZFRAMEWORK_LIB}, 仅导出头文件")
endif()
"""
    content = content.replace("__INCLUDE_DIRS__", include_dirs)
    content = content.replace("__REQUIRES__", requires)

    path = os.path.join(output, "CMakeLists.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print("[ok] 生成 %s" % path)


def gen_component_yml(output, modules):
    """生成 output/idf_component.yml (组件清单)。

    清单 name 必须与组件目录名一致(jzframework), 且 dependencies 只列
    【真实存在的 IDF 组件】。JZCore/JZAgent 等是组件内子目录, 不属于
    组件管理器可搜索的依赖, 故不写入, 否则组件管理器解析会失败。
    """
    lines = [
        "# JZFramework ESP-IDF 组件清单",
        "# 由 tools/release/esp32.py 自动生成, 请勿手工修改",
        "# 该文件供 IDF 组件管理器使用, 描述组件名/版本/依赖。",
        "# 组件名 jzframework 需与组件目录名一致, 才能被搜索到。",
        "",
        "name: jzframework",
        "version: \"1.0.0\"",
        "description: JZFramework headers and prebuilt libs for ESP32",
        "maintainer: JZFramework <maintainer@example.com>",
        "url: https://example.com/jzframework",
        "tags:",
        "  - framework",
        "  - esp32",
        "dependencies:",
        "  freertos:",
        "    version: \"*\"",
        "  driver:",
        "    version: \"*\"",
        "  esp_timer:",
        "    version: \"*\"",
        "  nvs_flash:",
        "    version: \"*\"",
        "  esp-tls:",
        "    version: \"*\"",
        "",
    ]

    path = os.path.join(output, "idf_component.yml")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print("[ok] 生成 %s" % path)


def detect_target(input_dir):
    """参考 jz_app, 从 input 目录自动探测 ESP-IDF 平台(IDF_TARGET)。

    探测优先级:
      1. input/CMakeLists.txt 中的 set(IDF_TARGET xxx ...)
      2. input/sdkconfig 中的 CONFIG_IDF_TARGET="xxx"
      3. 环境变量 IDF_TARGET
    返回平台名(如 esp32s3)或 None。
    """
    # 1. CMakeLists.txt
    cmake = os.path.join(input_dir, "CMakeLists.txt")
    if os.path.isfile(cmake):
        try:
            txt = open(cmake, encoding="utf-8", errors="ignore").read()
            m = re.search(r"set\s*\(\s*IDF_TARGET\s+([A-Za-z0-9_]+)", txt)
            if m:
                return m.group(1)
        except OSError:
            pass

    # 2. sdkconfig
    sdk = os.path.join(input_dir, "sdkconfig")
    if os.path.isfile(sdk):
        try:
            for line in open(sdk, encoding="utf-8", errors="ignore"):
                m = re.match(r'\s*CONFIG_IDF_TARGET="?([A-Za-z0-9_]+)"?\s*$', line)
                if m:
                    return m.group(1)
        except OSError:
            pass

    # 3. 环境变量
    env = os.environ.get("IDF_TARGET")
    if env:
        return env

    return None


def find_lib(input_dir, name):
    """在 input_dir 下递归查找名为 name 的文件, 返回首个命中路径。"""
    for root, _dirs, files in os.walk(input_dir):
        if name in files:
            return os.path.join(root, name)
    return None


def copy_libs_by_platform(input_dir, output):
    """参考 jz_app, 自动探测平台(IDF_TARGET)并把 JZFramework.a 拷到
    output/lib/<平台>/。

    约定:
      - input 为类似 jz_app 的 IDF 工程目录(含 CMakeLists.txt/sdkconfig
        或 build/), 平台由 IDF_TARGET 自动探测。
      - JZFramework.a 在 input 下某处(如 build/esp-idf/JZFramework/
        libJZFramework.a 或 build/JZFramework.a), 递归查找定位。
    返回平台名或 None。
    """
    if not input_dir or not os.path.isdir(input_dir):
        print("[skip] 未指定 --input 或目录不存在, 跳过库文件拷贝")
        return None

    target = detect_target(input_dir)
    if not target:
        print("[error] 无法从 input 探测到 IDF_TARGET(检查 CMakeLists.txt/"
              "sdkconfig 或设置环境变量 IDF_TARGET)", file=sys.stderr)
        return None
    print("[info] 探测到平台(IDF_TARGET): %s" % target)

    src = find_lib(input_dir, "JZFramework.a")
    if not src:
        print("[error] 未在 input 下找到 JZFramework.a: %s" % input_dir,
              file=sys.stderr)
        return None

    dst_dir = os.path.join(output, "lib", target)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, "JZFramework.a")
    shutil.copy2(src, dst)
    print("[ok] lib/%s/JZFramework.a <- %s" % (target, src))
    return target


def main():
    parser = argparse.ArgumentParser(description="JZFramework ESP32 发布脚本")
    parser.add_argument(
        "--output",
        default=os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "output"
        ),
        help="输出目录 (默认: <脚本目录>/output)",
    )
    parser.add_argument(
        "--input",
        default=None,
        help="编译产物目录 (默认: 无, 跳过库拷贝)。参考 jz_app, 自动探测 IDF_TARGET 平台, 递归查找 JZFramework.a 拷到 output/lib/<平台>/",
    )
    args = parser.parse_args()

    # 根目录固定为本脚本的上两级(即 JZFramework), 不从命令行传入
    root = os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    )
    output = os.path.abspath(args.output)
    input_dir = os.path.abspath(args.input) if args.input else None

    if not os.path.isdir(root):
        print("[error] 根目录不存在: %s" % root, file=sys.stderr)
        return 1

    print("== JZFramework ESP32 发布 ==")
    print("根目录 : %s" % root)
    print("输入目录: %s" % (input_dir if input_dir else "(无)"))
    print("输出目录: %s" % output)
    print("")

    # 1. 拷贝头文件
    os.makedirs(output, exist_ok=True)
    # 清空旧的 include 目录, 保证可重复执行
    inc_dir = os.path.join(output, "include")
    if os.path.isdir(inc_dir):
        shutil.rmtree(inc_dir)
    os.makedirs(inc_dir, exist_ok=True)

    total = 0
    present = []
    for m in MODULES:
        n = copy_module_headers(root, output, m)
        if n > 0:
            present.append(m)
            total += n
    print("")
    print("共拷贝 %d 个头文件, 来自 %d 个模块" % (total, len(present)))

    if not present:
        print("[error] 没有任何头文件被拷贝, 请检查模块路径", file=sys.stderr)
        return 1

    # 2. 根据平台拷贝库文件
    print("")
    print("-- 拷贝库文件 (按平台) --")
    lib_dir = os.path.join(output, "lib")
    if os.path.isdir(lib_dir):
        shutil.rmtree(lib_dir)
    target = copy_libs_by_platform(input_dir, output)
    if target:
        print("库文件平台: %s" % target)
    else:
        print("无库文件被拷贝")

    # 3. 生成组件配置文件
    print("")
    gen_cmakelists(output, present)
    gen_component_yml(output, present)

    print("")
    print("== 完成 == 输出位于: %s" % output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
