#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 code_agent 的构建产物(自包含目录 <build>/binary/bin/<CONFIG>)打包成 zip, 并通过系统
OpenSSH(scp/ssh) 推送到远端主机, 再在远端解压(可选执行部署命令)。

本机前提:
  - Windows: 自带 OpenSSH(ssh.exe/scp.exe, 位于 C:\\Windows\\System32\\OpenSSH)。
  - Linux/macOS: 系统自带 ssh/scp。
  - 认证走 ssh 自身机制: SSH agent、~/.ssh/config、或显式私钥(-i/--identity)。
    不支持在命令行直接写密码(OpenSSH 限制); 如需密码请自行用 sshpass,
    或在 ~/.ssh/config 里针对该主机配置好认证。

远端前提(仅推送时需要):
  - 目标目录可写(默认 /opt/jz/code_agent)。
  - 已安装 unzip(用于解压; 可用 extract_cmd 覆盖)。
  - 认证走 ssh 自身机制: SSH agent、~/.ssh/config、或显式私钥(identity)。
    不支持在命令行直接写密码(OpenSSH 限制); 如需密码请自行用 sshpass,
    或在 ~/.ssh/config 里针对该主机配置好认证。

本脚本无命令行参数: 所有配置写死在 parse_args() 里。直接运行即可:
  python tools/release/publish.py

当前固定行为: 打包 Release, 打包前先 cmake --build(code_agent+code_runtime)
并 `tsc -b js_wrapper`, 只产出本地 zip(不推送远端)。要推送远端, 改 parse_args()
里的 NO_PUSH=False 并视情况填 IDENTITY/DEPLOY_CMD 即可。
"""
import argparse
import fnmatch
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

DEFAULT_HOST = "120.77.183.99"
DEFAULT_USER = "root"
DEFAULT_PORT = 22
DEFAULT_REMOTE_DIR = "/opt/jz/code_agent"
DEFAULT_CONFIG = "Debug"

# 默认排除: Debug/Release 下的 .ilk 是增量链接文件, .pdb 是调试符号,
# 部署到远端运行时都不需要, 且体积巨大(pdb 常上百 MB), 默认排除。
DEFAULT_EXCLUDES = ["*.ilk", "*.pdb"]


def find_ssh_tool(name):
    """定位 ssh/scp 可执行文件; 优先 PATH, 其次 Windows OpenSSH 默认位置。"""
    found = shutil.which(name)
    if found:
        return found
    win = r"C:\Windows\System32\OpenSSH\{}.exe".format(name)
    if os.path.isfile(win):
        return win
    return None


def find_pscp():
    """定位 PuTTY 的 pscp.exe(PuTTY 安装目录或 PATH)。"""
    found = shutil.which("pscp")
    if found:
        return found
    for p in (r"C:\Program Files\PuTTY\pscp.exe",
              r"C:\Program Files (x86)\PuTTY\pscp.exe",
              r"C:\Users\{}\AppData\Local\Microsoft\WinGet\Links\pscp.exe".format(
                  os.environ.get("USERNAME", ""))):
        if os.path.isfile(p):
            return p
    return None


def resolve_src(args):
    if args.src:
        return os.path.abspath(args.src)
    return os.path.join(REPO_ROOT, "build", "binary", "bin", args.config)


def find_cmake():
    return shutil.which("cmake")


def find_tsc(build_dir):
    """优先用 build 下捆绑的 ts_compiler, 否则回退到 PATH 的 tsc。"""
    cands = glob.glob(os.path.join(build_dir, "**",
                                   "ts_compiler", "tsc", "tsc.exe"),
                      recursive=True)
    if cands:
        return cands[0]
    return shutil.which("tsc")


def run_tsc(build_dir):
    """tsc -b js_wrapper, 生成各包 dist(供 copy_jz_js 打包成部署的 js_wrapper 树)。"""
    tsc = find_tsc(build_dir)
    if not tsc:
        print("[publish] ERROR: tsc not found (no bundled ts_compiler under "
              "build/, and tsc not on PATH)", file=sys.stderr)
        return 1
    wrap = os.path.join(REPO_ROOT, "JZJSEngine", "js_wrapper")
    cmd = [tsc, "-b", wrap]
    print("[publish] tsc:\n  " + " ".join(cmd))
    return subprocess.run(cmd).returncode


def run_build(args):
    """cmake --build <build-dir> --config <config> --target <build-target>。

    可指定多个 target(--build-target 可重复); 默认 code_agent + code_runtime
    (code_agent 以子进程方式调用 code_runtime 跑 TS 运行时, 部署包需二者齐全)。
    """
    cmake = find_cmake()
    if not cmake:
        print("[publish] ERROR: cmake not found on PATH", file=sys.stderr)
        return 1
    targets = args.build_target or ["code_agent", "code_runtime"]
    rc = 0
    for t in targets:
        cmd = [cmake, "--build", os.path.abspath(args.build_dir),
               "--config", args.config, "--target", t]
        print("[publish] build:\n  " + " ".join(cmd))
        if subprocess.run(cmd).returncode != 0:
            rc = 1
    return rc


def bundle_ts_compiler(src):
    """code_agent 运行时依赖 <exe>/ts_compiler/tsc/tsc.exe(默认来自 E:/libs/ts_compiler)。

    CMake 不负责拷贝它, 所以打包前若 bin/<config> 下缺 ts_compiler, 就从候选位置
    拷一份进去(只拷一次, 已存在则跳过)。找不到仅告警, 不阻断打包。
    """
    dst = os.path.join(src, "ts_compiler")
    if os.path.isdir(dst):
        return True
    candidates = [
        "E:/libs/ts_compiler",
        os.path.join(REPO_ROOT, "build", "binary", "bin", "Debug", "ts_compiler"),
    ]
    for c in candidates:
        if os.path.isdir(c):
            shutil.copytree(c, dst)
            print("[publish] bundled ts_compiler <- {}".format(c))
            return True
    print("[publish] WARN: ts_compiler not found (tried {}); code_agent TS "
          "features need <exe>/ts_compiler/tsc/tsc.exe".format(candidates),
          file=sys.stderr)
    return False


def make_archive(src, zip_path, top_folder, excludes):
    """把 src 整目录打成 zip。

    top_folder 为空时, 压缩包内顶层直接是 src 下的文件(扁平);
    否则每层路径都带 `<top_folder>/` 前缀, 解压后落在 `<remote-dir>/<top_folder>/`。
    返回加入的文件数; src 不存在返回 None。
    """
    if not os.path.isdir(src):
        print("[publish] ERROR: source dir not found: {}".format(src), file=sys.stderr)
        return None
    parent = os.path.dirname(src.rstrip(os.sep))
    n = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _dirs, files in os.walk(src):
            for f in sorted(files):
                fp = os.path.join(root, f)
                inner = os.path.relpath(fp, parent).replace(os.sep, "/")
                if top_folder:
                    inner = "{}/{}".format(top_folder, inner)
                if any(fnmatch.fnmatch(inner, pat) for pat in excludes):
                    continue
                z.write(fp, inner)
                n += 1
    return n


def sh_quote(s):
    """对远端 shell 字符串做单引号转义。"""
    return "'" + s.replace("'", "'\\''") + "'"


def scp_push(scp, host, port, user, identity, local, remote_path,
             ssh_opts, strict_host):
    remote_spec = "{}@{}:{}".format(user, host, remote_path)
    cmd = [scp, "-P", str(port),
           "-o", "StrictHostKeyChecking={}".format(strict_host)]
    if identity:
        cmd += ["-i", identity]
    cmd += list(ssh_opts) + [local, remote_spec]
    print("[publish] scp:\n  " + " ".join(cmd))
    return subprocess.run(cmd).returncode


def remote_run(ssh, host, port, user, identity, cmd_str,
               ssh_opts, strict_host):
    base = [ssh, "-p", str(port),
            "-o", "StrictHostKeyChecking={}".format(strict_host)]
    if identity:
        base += ["-i", identity]
    base += list(ssh_opts) + ["{}@{}".format(user, host), cmd_str]
    print("[publish] ssh:\n  " + " ".join(base))
    return subprocess.run(base).returncode


def pscp_push(pscp, host, port, user, identity, local, remote_path, batch=True):
    """用 PuTTY 的 pscp 上传, 支持 .ppk 私钥。

    -i <ppk>  私钥(PuTTY 格式, OpenSSH 不能直接用)
    -P <port> 端口(注意 pscp 是大写 P)
    -batch    非交互(首次未知 host key 时直接失败而非卡在确认提示)
    远端目录需已存在(pscp 不会自动建目录)。
    """
    remote_spec = "{}@{}:{}".format(user, host, remote_path)
    cmd = [pscp, "-i", identity, "-P", str(port)]
    if batch:
        cmd.append("-batch")
    cmd += [local, remote_spec]
    print("[publish] pscp:\n  " + " ".join(cmd))
    return subprocess.run(cmd).returncode


def parse_args():
    """所有参数写死, 不接受命令行参数。改这里即可调整行为。

    当前固定: 打包 Release, 打包前先 cmake --build(code_agent+code_runtime) 并
    `tsc -b js_wrapper`, 然后用 pscp(PuTTY) + .ppk 私钥上传 zip 到
    /root/work/wwwroot/download, 不解压(NO_PUSH=False, NO_EXTRACT=True,
    USE_PSCP=True)。改 IDENTITY/REMOTE_DIR/PACKAGE_NAME 调整位置。
    """
    ns = argparse.Namespace()
    ns.config = "Release"
    ns.src = None                       # None -> <repo>/build/binary/bin/<config>
    ns.host = DEFAULT_HOST              # 120.77.183.99
    ns.port = DEFAULT_PORT              # 22
    ns.user = DEFAULT_USER              # root
    ns.use_pscp = True                  # True -> 用 PuTTY pscp + .ppk 上传
    ns.identity = r"F:\xiong\阿里云\xiongqiumin.ppk"   # PuTTY 格式私钥
    ns.remote_dir = "/root/work/wwwroot/download"      # 远端目标目录(需已存在)
    ns.remote_name = None               # 压缩包顶层目录名(默认=config)
    ns.flat = False
    ns.package_name = None              # 默认 code_agent_<config>.zip
    ns.exclude = []
    ns.include_debug = False            # False -> 排除 *.pdb/*.ilk
    ns.no_extract = True               # True -> 上传后不解压, zip 直接留在远端指定位置
    ns.extract_cmd = "unzip -o"
    ns.deploy_cmd = None                # 解压后执行的远端命令(推送时按需填)
    ns.ssh_opts = None
    ns.insecure_hostkey = False
    ns.keep_archive = False
    ns.build = True
    ns.build_dir = os.path.join(REPO_ROOT, "build")
    ns.build_target = ["code_agent", "code_runtime"]
    ns.build_js = True
    ns.no_push = False                  # False -> scp 上传到远端(不解压)
    ns.out = None                       # None -> 临时目录(推送后删除本地 zip)
    ns.dry_run = False
    return ns


def main():
    args = parse_args()

    src = resolve_src(args)
    top_folder = "" if args.flat else (args.remote_name or args.config)
    package_name = args.package_name or "code_agent_{}.zip".format(args.config)

    excludes = list(args.exclude)
    if not args.include_debug:
        excludes += DEFAULT_EXCLUDES

    ssh_opts = args.ssh_opts.split() if args.ssh_opts else []
    strict_host = "no" if args.insecure_hostkey else "accept-new"

    # 本地打包目录: 显式 --out > --no-push 时的稳定目录 > 临时目录
    if args.out:
        out_dir = os.path.abspath(args.out)
    elif args.no_push:
        out_dir = os.path.join(REPO_ROOT, "tools", "release", "out")
    else:
        out_dir = tempfile.mkdtemp(prefix="publish_")
    zip_path = os.path.join(out_dir, package_name)

    remote_path = "{}/{}".format(args.remote_dir.rstrip("/"), package_name)

    if args.dry_run:
        print("[publish] DRY-RUN plan:")
        print("  build-js   : {}".format(args.build_js))
        print("  build      : {}".format(args.build))
        print("  src        : {}".format(src))
        print("  archive    : {} (excludes: {})".format(zip_path, excludes))
        print("  host       : {}@{}:{}".format(args.user, args.host, args.port))
        print("  remote zip : {}".format(remote_path))
        print("  extract    : {}".format("no" if args.no_extract else "yes"))
        print("  deploy-cmd : {}".format(args.deploy_cmd))
        return 0

    # 构建步骤(先于打包): 可选先编 JS dist, 再 cmake --build 出 code_agent。
    if args.build_js:
        if run_tsc(args.build_dir) != 0:
            print("[publish] ERROR: tsc build failed", file=sys.stderr)
            return 1
    if args.build:
        if run_build(args) != 0:
            print("[publish] ERROR: cmake build failed", file=sys.stderr)
            return 1

    ssh = scp = pscp = None
    if not args.no_push:
        if args.use_pscp:
            pscp = find_pscp()
            if not pscp:
                print("[publish] ERROR: pscp not found (PuTTY pscp.exe on PATH "
                      "or in C:\\Program Files\\PuTTY).", file=sys.stderr)
                return 2
        else:
            scp = find_ssh_tool("scp")
            if not scp:
                print("[publish] ERROR: scp not found on PATH. "
                      "Install OpenSSH (Windows: 'Add OpenSSH Client' optional feature).",
                      file=sys.stderr)
                return 2
            # 仅当需要在远端执行命令(解压/部署)时才需要 ssh
            if (not args.no_extract) or args.deploy_cmd:
                ssh = find_ssh_tool("ssh")
                if not ssh:
                    print("[publish] ERROR: ssh not found on PATH (needed for "
                          "remote extract/deploy).", file=sys.stderr)
                    return 2

    if not os.path.isdir(src):
        print("[publish] ERROR: source dir not found: {}".format(src),
              file=sys.stderr)
        return 2

    # code_agent 运行时需要同目录下的 ts_compiler; 打包前补齐(若 CMake 没拷)。
    bundle_ts_compiler(src)

    os.makedirs(out_dir, exist_ok=True)
    print("[publish] packaging: {}  (top folder: {})".format(
        src, top_folder or "<none>"))
    n = make_archive(src, zip_path, top_folder, excludes)
    if n is None:
        return 2
    size_mb = os.path.getsize(zip_path) / (1024.0 * 1024.0)
    print("[publish] archive: {}  ({} files, {:.1f} MB)".format(
        zip_path, n, size_mb))

    # 只打包不推送
    if args.no_push:
        print("[publish] OK (no-push) -> {}".format(zip_path))
        return 0

    # 1) 上传
    if args.use_pscp:
        if pscp_push(pscp, args.host, args.port, args.user, args.identity,
                     zip_path, remote_path) != 0:
            print("[publish] ERROR: pscp upload failed", file=sys.stderr)
            return 1
    else:
        if scp_push(scp, args.host, args.port, args.user, args.identity,
                    zip_path, remote_path, ssh_opts, strict_host) != 0:
            print("[publish] ERROR: scp failed", file=sys.stderr)
            return 1

    # 2) 远端解压
    if not args.no_extract:
        rd = args.remote_dir.rstrip("/")
        pkg = "{}/{}".format(rd, package_name)
        # 建目录 -> 进入 -> 解压 -> 删包
        cmd = ("mkdir -p {rd} && cd {rd} && {ec} {pkg} && rm -f {pkg}".format(
            rd=sh_quote(rd), ec=args.extract_cmd, pkg=sh_quote(pkg)))
        if remote_run(ssh, args.host, args.port, args.user, args.identity,
                      cmd, ssh_opts, strict_host) != 0:
            print("[publish] ERROR: remote extract failed", file=sys.stderr)
            return 1
        print("[publish] extracted -> {}".format(rd))

    # 3) 部署命令
    if args.deploy_cmd:
        if remote_run(ssh, args.host, args.port, args.user, args.identity,
                      args.deploy_cmd, ssh_opts, strict_host) != 0:
            print("[publish] ERROR: deploy-cmd failed", file=sys.stderr)
            return 1
        print("[publish] deploy-cmd done")

    if not args.keep_archive:
        os.remove(zip_path)
        if os.path.abspath(out_dir) != os.path.abspath(
                os.path.join(REPO_ROOT, "tools", "release", "out")):
            shutil.rmtree(out_dir, ignore_errors=True)
    print("[publish] OK -> {}@{}:{}{}".format(
        args.user, args.host, remote_path,
        "" if args.no_extract else " (extracted)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
