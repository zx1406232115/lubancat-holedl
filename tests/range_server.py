#!/usr/bin/env python3
"""
range_server.py - 支持 HTTP Range 的测试服务器

为什么要自己写一个：
    python3 -m http.server 的 SimpleHTTPRequestHandler 不支持 Range 请求，
    收到 `Range: bytes=0-262143` 会直接回 200 + 整个文件。
    而断点续传必须依赖 206 Partial Content，所以测试要用真正的 Range 服务器。

用法:
    python3 tests/range_server.py <目录> <端口> [每秒字节数]

第三个参数用来限速（字节/秒），方便演示「下到一半 Ctrl-C 再续传」：
    python3 tests/range_server.py . 8000 131072   # 限速 128KiB/s

它实现了：
    - HEAD             -> 返回 Content-Length，不返回 body（holedl 用它探测大小）
    - GET + Range      -> 206 + Content-Range（holedl 用它分块定点写）
    - GET 无 Range     -> 200 全量
"""
import os
import re
import sys
import time
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
# 每秒最多发送的字节数，0 表示不限速
THROTTLE = int(sys.argv[3]) if len(sys.argv) > 3 else 0

RANGE_RE = re.compile(r"bytes=(\d*)-(\d*)")


def throttled_write(handler, data):
    """按 THROTTLE 限速写出，让「下到一半」这个时刻可复现"""
    if THROTTLE <= 0:
        handler.wfile.write(data)
        return
    # 按 16KiB 切片发送，每片之间 sleep，得到平滑的限速效果
    step = 16 * 1024
    for i in range(0, len(data), step):
        chunk = data[i:i + step]
        handler.wfile.write(chunk)
        handler.wfile.flush()
        time.sleep(len(chunk) / float(THROTTLE))


class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        # 打到 stderr，避免污染测试输出
        pass

    # ---------- 工具 ----------
    def _resolve(self):
        rel = self.path.split("?", 1)[0].lstrip("/")
        full = os.path.abspath(os.path.join(ROOT, rel))
        # 防止路径穿越
        if not full.startswith(ROOT) or not os.path.isfile(full):
            return None
        return full

    def _parse_range(self, size):
        hdr = self.headers.get("Range")
        if not hdr:
            return None
        m = RANGE_RE.match(hdr.strip())
        if not m:
            return None
        s, e = m.group(1), m.group(2)
        if s == "" and e == "":
            return None
        if s == "":
            # bytes=-N 表示最后 N 字节
            n = int(e)
            if n <= 0:
                return None
            start = max(0, size - n)
            end = size - 1
        else:
            start = int(s)
            end = int(e) if e != "" else size - 1
        if start >= size:
            return "unsatisfiable"
        end = min(end, size - 1)
        if end < start:
            return None
        return (start, end)

    # ---------- 方法 ----------
    def do_HEAD(self):
        path = self._resolve()
        if path is None:
            self.send_error(404, "Not Found")
            return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        path = self._resolve()
        if path is None:
            self.send_error(404, "Not Found")
            return

        size = os.path.getsize(path)
        rng = self._parse_range(size)

        if rng == "unsatisfiable":
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % size)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        if rng is None:
            # 全量
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(path, "rb") as f:
                while True:
                    chunk = f.read(64 * 1024)
                    if not chunk:
                        break
                    throttled_write(self, chunk)
            return

        start, end = rng
        length = end - start + 1
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()

        with open(path, "rb") as f:
            f.seek(start)
            left = length
            while left > 0:
                chunk = f.read(min(64 * 1024, left))
                if not chunk:
                    break
                throttled_write(self, chunk)
                left -= len(chunk)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    srv = Server(("127.0.0.1", PORT), RangeHandler)
    sys.stderr.write("range_server: serving %s on 127.0.0.1:%d\n" % (ROOT, PORT))
    sys.stderr.flush()
    srv.serve_forever()
