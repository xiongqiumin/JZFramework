#!/usr/bin/env python3
# net_test_server.py
#
# 本地一体化网络测试服务, 配合 test/net 下的 C++ 测试在离线环境验证:
#   - test_proxy.cpp   : http 目标经代理转发 + https 目标经代理 CONNECT 隧道
#   - test_net_compress: HTTP Content-Encoding 解压 (gzip / deflate / plain)
#
# 本脚本同时启动三个服务:
#   1) 目标 HTTP  站点  http://127.0.0.1:8080   (Flask; 另提供 /gzip /deflate /plain 压缩端点)
#   2) 目标 HTTPS 站点  https://127.0.0.1:8443  (Flask + 运行时生成的自签名证书)
#   3) HTTP 代理         http://127.0.0.1:7001   (原生 socket, 支持 CONNECT 隧道)
#
# 说明: Flask (WSGI) 无法处理 CONNECT 隧道, 所以代理本体用标准库 socket 实现;
#       Flask 仅用于提供"被访问的目标站点"与压缩端点。证书每次运行自动生成, 无需预置文件。
#       对应 C++ 侧已 setSsl(sslSkipVerify=true), 故自签名证书可被接受。
#
# 依赖: flask, cryptography
# 用法: python net_test_server.py        (Ctrl+C 退出)

import socket
import threading
import datetime
import sys
import gzip
import zlib

from flask import Flask, request, Response

HTTP_TARGET_HOST = "127.0.0.1"
HTTP_TARGET_PORT = 8080
HTTPS_TARGET_PORT = 8443
PROXY_HOST = "127.0.0.1"
PROXY_PORT = 7001


# ---------------------------------------------------------------------------
# 1. 目标站点 (Flask): 返回可识别的响应体, 便于断言代理转发/隧道是否成功
# ---------------------------------------------------------------------------
def make_target_app(label):
    app = Flask("target_" + label)

    @app.route("/", defaults={"p": ""})
    @app.route("/<path:p>")
    def index(p):
        body = (
            "PROXY_TEST_OK\r\n"
            "label=" + label + "\r\n"
            "scheme=" + ("https" if label == "https" else "http") + "\r\n"
            "path=/" + p + "\r\n"
            "host_header=" + request.headers.get("Host", "") + "\r\n"
        )
        return Response(body, mimetype="text/plain")

    return app


# ---------------------------------------------------------------------------
# 1b. 压缩测试端点（供 test_net_compress 验证客户端 HTTP Content-Encoding 解压）
#   /plain   返回原始明文基准
#   /gzip    返回 gzip 压缩体 (Content-Encoding: gzip)
#   /deflate 返回 zlib 封装压缩体 (Content-Encoding: deflate, 即 RFC1950)
# ---------------------------------------------------------------------------
COMPRESS_PAYLOAD = (b"JZNet http compression end-to-end test payload line.\n" * 1500)


def make_compress_routes(app):
    @app.route("/gzip")
    def _gzip():
        return Response(
            gzip.compress(COMPRESS_PAYLOAD),
            headers={"Content-Encoding": "gzip", "Content-Type": "text/plain"},
        )

    @app.route("/deflate")
    def _deflate():
        return Response(
            zlib.compress(COMPRESS_PAYLOAD),
            headers={"Content-Encoding": "deflate", "Content-Type": "text/plain"},
        )

    @app.route("/plain")
    def _plain():
        return Response(COMPRESS_PAYLOAD, mimetype="text/plain")

    @app.route("/auto")
    def _auto():
        # 内容协商：仅当客户端显式声明 Accept-Encoding 时才压缩响应，否则回退明文。
        # C++ 客户端 (JZNetworkAccessManager) 现已默认发送 "Accept-Encoding: gzip, deflate"，
        # 故经它访问本路由会得到 gzip 压缩体 (Content-Encoding: gzip)；
        # 不带该头（如 curl 直连）则回退明文。用于端到端验证客户端头是否真的发出。
        ae = (request.headers.get("Accept-Encoding") or "").lower()
        if "gzip" in ae:
            return Response(
                gzip.compress(COMPRESS_PAYLOAD),
                headers={"Content-Encoding": "gzip", "Content-Type": "text/plain"},
            )
        if "deflate" in ae:
            return Response(
                zlib.compress(COMPRESS_PAYLOAD),
                headers={"Content-Encoding": "deflate", "Content-Type": "text/plain"},
            )
        return Response(COMPRESS_PAYLOAD, mimetype="text/plain")


# ---------------------------------------------------------------------------
# 2. 运行时生成自签名证书 (CN=127.0.0.1, SAN 含 127.0.0.1 / localhost)
# ---------------------------------------------------------------------------
def gen_self_signed_cert():
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import ipaddress

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = issuer = x509.Name(
        [x509.NameAttribute(NameOID.COMMON_NAME, u"127.0.0.1")]
    )
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(
            x509.SubjectAlternativeName(
                [
                    x509.DNSName(u"localhost"),
                    x509.IPAddress(ipaddress.ip_address(u"127.0.0.1")),
                ]
            ),
            critical=False,
        )
        .sign(key, hashes.SHA256())
    )
    cert_pem = cert.public_bytes(serialization.Encoding.PEM)
    key_pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    )
    return cert_pem, key_pem


# ---------------------------------------------------------------------------
# 3. HTTP 代理 (原生 socket): 支持 CONNECT 隧道 + 绝对 URI 转发
# ---------------------------------------------------------------------------
def recv_until_headers(sock):
    """读取到 \r\n\r\n 为止, 返回含头的完整缓冲(可能含部分 body)。"""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if len(buf) > 1 << 20:
            break
    return buf


def parse_target_host(target, header_lines):
    """从绝对 URI 或 Host 头解析 (host, port)。"""
    if target.lower().startswith("http://"):
        from urllib.parse import urlparse

        u = urlparse(target)
        return u.hostname, (u.port or 80)
    for ln in header_lines[1:]:
        if ln.lower().startswith(b"host:"):
            v = ln.split(b":", 1)[1].strip().decode("latin1")
            if ":" in v:
                h, p = v.rsplit(":", 1)
                return h, int(p)
            return v, 80
    raise ValueError("cannot determine target host")


def do_tunnel(client, host, port):
    """CONNECT 隧道: 连上目标, 回 200, 之后双向盲转发(支持 keep-alive 复用)。"""
    try:
        upstream = socket.create_connection((host, port), timeout=10)
    except Exception:
        client.sendall(b"HTTP/1.1 502 Bad Gateway\r\n\r\n")
        return
    client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")

    def pipe(src, dst):
        try:
            while True:
                buf = src.recv(65536)
                if not buf:
                    break
                dst.sendall(buf)
        except Exception:
            pass
        try:
            dst.shutdown(socket.SHUT_WR)
        except Exception:
            pass

    t1 = threading.Thread(target=pipe, args=(client, upstream), daemon=True)
    t2 = threading.Thread(target=pipe, args=(upstream, client), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    for s in (client, upstream):
        try:
            s.close()
        except Exception:
            pass


def forward_http(client, method, target, version, header_lines, rest, host, port):
    """绝对 URI 转发: 改写请求行为 origin-form, 加 Connection: close, 转发并回传响应。"""
    from urllib.parse import urlparse

    path = target
    if target.lower().startswith("http://"):
        u = urlparse(target)
        path = u.path or "/"
        if u.query:
            path += "?" + u.query

    new_headers = []
    client_close = False
    for ln in header_lines[1:]:
        low = ln.lower()
        if low.startswith(b"proxy-"):
            continue  # 去掉 Proxy-* 头
        if low.startswith(b"connection:"):
            client_close = b"close" in low
            continue
        new_headers.append(ln)
    new_headers.append(b"Connection: close")  # 代理->目标 不用 keep-alive, 读到 EOF 即结束

    req = (method + " " + path + " " + version + "\r\n").encode("latin1")
    req += b"\r\n".join(new_headers) + b"\r\n\r\n"
    req += rest

    upstream = socket.create_connection((host, port), timeout=10)
    upstream.sendall(req)

    while True:
        buf = upstream.recv(65536)
        if not buf:
            break
        client.sendall(buf)
    upstream.close()

    # 客户端若要求关闭则结束本连接, 否则继续读下一个请求 (keep-alive 复用)
    return not client_close


def handle_proxy_client(client):
    try:
        while True:
            data = recv_until_headers(client)
            if not data:
                break
            head, _, rest = data.partition(b"\r\n\r\n")
            lines = head.split(b"\r\n")
            parts = lines[0].decode("latin1").split(" ")
            if len(parts) < 3:
                break
            method, target, version = parts[0], parts[1], parts[2]
            if method.upper() == "CONNECT":
                host, _, port = target.partition(":")
                do_tunnel(client, host, int(port))
                break  # 隧道由 pipe 线程接管, 此处退出循环
            else:
                host, port = parse_target_host(target, lines)
                keep = forward_http(
                    client, method, target, version, lines, rest, host, port
                )
                if not keep:
                    break
    except Exception:
        pass
    finally:
        try:
            client.close()
        except Exception:
            pass


def start_proxy():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((PROXY_HOST, PROXY_PORT))
    srv.listen(128)
    print(f"[proxy] 监听 http://{PROXY_HOST}:{PROXY_PORT} (支持 CONNECT 隧道)")
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle_proxy_client, args=(conn,), daemon=True).start()


# ---------------------------------------------------------------------------
# 启动
# ---------------------------------------------------------------------------
def main():
    # 目标 HTTP 站点
    http_app = make_target_app("http")
    make_compress_routes(http_app)  # /gzip /deflate /plain 压缩端点
    threading.Thread(
        target=lambda: http_app.run(
            host=HTTP_TARGET_HOST, port=HTTP_TARGET_PORT, threaded=True
        ),
        daemon=True,
    ).start()
    print(f"[http-target]  http://{HTTP_TARGET_HOST}:{HTTP_TARGET_PORT}")

    # 目标 HTTPS 站点 (自签名证书, C++ 侧跳过校验)
    cert_pem, key_pem = gen_self_signed_cert()
    import ssl as _ssl
    import tempfile
    import os

    cert_file = tempfile.NamedTemporaryFile(  # 证书/私钥落临时文件供 ssl 加载
        delete=False, suffix=".pem"
    )
    key_file = tempfile.NamedTemporaryFile(delete=False, suffix=".pem")
    cert_file.write(cert_pem)
    key_file.write(key_pem)
    cert_file.close()
    key_file.close()

    ctx = _ssl.SSLContext(_ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert_file.name, keyfile=key_file.name)
    https_app = make_target_app("https")
    threading.Thread(
        target=lambda: https_app.run(
            host=HTTP_TARGET_HOST,
            port=HTTPS_TARGET_PORT,
            threaded=True,
            ssl_context=ctx,
        ),
        daemon=True,
    ).start()
    print(f"[https-target] https://{HTTP_TARGET_HOST}:{HTTPS_TARGET_PORT} (自签名证书)")

    # HTTP 代理
    threading.Thread(target=start_proxy, daemon=True).start()

    print("\n本地测试服务已就绪。另开终端运行 test_proxy.exe 即可。Ctrl+C 退出。\n")
    try:
        while True:
            threading.Event().wait(3600)
    except KeyboardInterrupt:
        print("\n[退出] 停止测试服务。")


if __name__ == "__main__":
    main()
