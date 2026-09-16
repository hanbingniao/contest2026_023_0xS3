#!/usr/bin/env python3
"""路线 1：串口 LLM relay（开发机侧）。

板端 velaops 串口隧道（`velaops tunnel`）把 ai_agent 的 LLM HTTP 请求经
USB 串口以 `@@VOPREQ_BEGIN/...@@VOPREQ_END` 帧发出；本脚本读串口、解帧、
把请求转发给真实 LLM 转发器，再把响应经 NSH 写回板内 `/tmp/vop-in.b64`，
板端隧道据此回写 ai_agent。这样 12KB 级 LLM 请求完全绕开 WiFi。

用法：
    PORT=/dev/ttyACM0 FORWARD_HOST=192.168.71.90 FORWARD_PORT=28792 \
      python3 docs/tools/serial_llm_relay.py
"""

from __future__ import annotations

import base64
import os
import select
import socket
import sys
import threading
import queue
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial_push  # noqa: E402

PORT = os.environ.get("PORT", "/dev/ttyACM0")
FORWARD_HOST = os.environ.get("FORWARD_HOST", "192.168.71.90")
FORWARD_PORT = int(os.environ.get("FORWARD_PORT", "28792"))
PROXY_HOST = os.environ.get("PROXY_HOST", "192.168.71.90")
PROXY_PORT = int(os.environ.get("PROXY_PORT", "28790"))
B64_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")
B64_REMOTE = "/tmp/vop-in.b64"
READY_REMOTE = "/tmp/vop-in.ready"
_ECHO = os.environ.get("RELAY_ECHO", "0") == "1"


def _target_for(request: bytes) -> tuple[str, int]:
    """按请求路径选择后端：chat/completions 走 LLM 转发器，其余走 Proxy。"""
    try:
        first = request.split(b"\r\n", 1)[0].split()
        path = first[1].decode("ascii", "replace") if len(first) >= 2 else ""
    except Exception:  # noqa: BLE001
        path = ""
    if path.startswith("/v1/chat/completions"):
        return FORWARD_HOST, FORWARD_PORT
    return PROXY_HOST, PROXY_PORT


def _forward(request: bytes) -> bytes:
    """把原始 HTTP 请求按路径转发给 LLM 转发器或 Proxy，返回原始响应。"""
    host, port = _target_for(request)
    with socket.create_connection((host, port), timeout=120) as s:
        s.sendall(request)
        s.shutdown(socket.SHUT_WR)
        chunks = []
        s.settimeout(120)
        while True:
            try:
                data = s.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks)


def _reader(fd: int, frames: "queue.Queue[bytes]") -> None:
    buffer = bytearray()

    while True:
        readable, _, _ = select.select([fd], [], [], 0.2)
        if not readable:
            continue
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        except OSError:
            break
        if not data:
            break

        buffer += data
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            line = raw.strip(b"\r").strip()
            marker = line.find(b"@@VOPREQ ")
            if marker >= 0:
                # 控制台多线程输出可能把日志拼在帧前，只取标记之后的 base64。
                payload = bytes(
                    c for c in line[marker + len(b"@@VOPREQ "):] if chr(c) in B64_CHARS)
                if payload:
                    try:
                        request = base64.b64decode(payload, validate=False)
                        if request[:4] in (b"POST", b"GET ", b"HEAD"):
                            frames.put(request)
                        else:
                            sys.stderr.write(
                                f"[relay] 帧损坏被忽略（{len(payload)}b64）\n")
                            sys.stderr.flush()
                    except Exception as exc:  # noqa: BLE001
                        sys.stderr.write(f"[relay] 解帧失败: {exc}\n")
                        sys.stderr.flush()
            elif _ECHO and line:
                sys.stdout.write(raw.decode(errors="replace") + "\n")
                sys.stdout.flush()
            if len(buffer) > 4 * 1024 * 1024:
                buffer.clear()
        if len(buffer) > 4 * 1024 * 1024:
            buffer.clear()


def _push_commands(fd: int, payload: bytes, remote: str) -> None:
    commands = serial_push.build_commands(payload, remote)
    char_delay = float(os.environ.get("PUSH_CHAR_DELAY", "0.001"))
    for command in commands:
        # 逐字符发送：整行快发会被 NSH 合并/截断（实测出现 "no matching '"）。
        for ch in command:
            os.write(fd, ch.encode("ascii"))
            time.sleep(char_delay)
        os.write(fd, b"\n")
        time.sleep(float(os.environ.get("PUSH_LINE_DELAY", "0.06")))


def main() -> int:
    fd = serial_push._open_raw_tty(PORT)
    frames: "queue.Queue[bytes]" = queue.Queue()

    # 板端 HMAC 需要有效系统时间；隧道只搬 HTTP，NTP 仍走 WiFi 且常掉线。
    # 启动时直接由开发机注入当前 Unix 时间，保证后续请求可签名。
    if os.environ.get("SET_TIME", "1") == "1":
        time.sleep(float(os.environ.get("SET_TIME_DELAY", "6")))
        epoch = int(time.time())
        os.write(fd, f"velaops set-time {epoch}\n".encode())
        time.sleep(0.5)
        serial_push._drain(fd)
        sys.stderr.write(f"[relay] 已注入板端时间 {epoch}\n")
        sys.stderr.flush()

    thread = threading.Thread(target=_reader, args=(fd, frames), daemon=True)
    thread.start()

    sys.stderr.write(
        f"[relay] 已就绪：串口 {PORT} -> forwarder {FORWARD_HOST}:{FORWARD_PORT}\n")
    sys.stderr.flush()

    while True:
        request = frames.get()
        sys.stderr.write(f"[relay] 收到板端请求 {len(request)} 字节\n")
        sys.stderr.flush()
        try:
            response = _forward(request)
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write(f"[relay] 转发失败: {exc}\n")
            response = b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"

        encoded = base64.b64encode(response) + b"\n"
        _push_commands(fd, encoded, B64_REMOTE)
        serial_push._drain(fd)
        os.write(fd, b"echo x > " + READY_REMOTE.encode() + b"\n")
        time.sleep(0.1)
        serial_push._drain(fd)
        sys.stderr.write(f"[relay] 已回写响应 {len(response)} 字节\n")
        sys.stderr.flush()


if __name__ == "__main__":
    raise SystemExit(main())
