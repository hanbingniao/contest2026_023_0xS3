#!/usr/bin/env python3
"""路线 1：串口 LLM relay（开发机侧）。

板端 `velaops tunnel` 把 ai_agent / 看板的 HTTP 请求切块发给本脚本：
    @@VF <xid> <nchunks> <b64len>
    @@VC <xid> <seq> <crc16> <base64块>
本脚本校验 CRC、收集全部块后回 ACK（板端停止重发），缺块则回 NACK（板端只重发
缺块）。随后按路径把原始 HTTP 转发给本机 Proxy 或 LLM 转发器，并把响应经 NSH
写回板内 /tmp/vop-in.b64，置位 /tmp/vop-in.ready。这样单路 USB CDC 上的偶发插帧
也能自愈，且不需要额外硬件或改动上游。

用法：
    PORT=/dev/ttyACM0 SET_TIME=1 \
      PROXY_HOST=127.0.0.1 PROXY_PORT=28790 \
      FORWARD_HOST=127.0.0.1 FORWARD_PORT=28792 \
      python3 docs/tools/serial_llm_relay.py
"""

from __future__ import annotations

import base64
import os
import queue
import select
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial_push  # noqa: E402

PORT = os.environ.get("PORT", "/dev/ttyACM0")
FORWARD_HOST = os.environ.get("FORWARD_HOST", "127.0.0.1")
FORWARD_PORT = int(os.environ.get("FORWARD_PORT", "28792"))
PROXY_HOST = os.environ.get("PROXY_HOST", "127.0.0.1")
PROXY_PORT = int(os.environ.get("PROXY_PORT", "28790"))
B64_REMOTE = "/tmp/vop-in.b64"
READY_REMOTE = "/tmp/vop-in.ready"
ACK_REMOTE = "/tmp/vop-ack"
NACK_REMOTE = "/tmp/vop-nack"

MAX_ROUNDS = int(os.environ.get("RELAY_MAX_ROUNDS", "6"))
ROUND_SECONDS = float(os.environ.get("RELAY_ROUND_SECONDS", "3"))
_ECHO = os.environ.get("RELAY_ECHO", "0") == "1"


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


def _target_for(request: bytes) -> tuple[str, int]:
    try:
        first = request.split(b"\r\n", 1)[0].split()
        path = first[1].decode("ascii", "replace") if len(first) >= 2 else ""
    except Exception:  # noqa: BLE001
        path = ""
    if path.startswith("/v1/chat/completions"):
        return FORWARD_HOST, FORWARD_PORT
    return PROXY_HOST, PROXY_PORT


def _forward(request: bytes) -> bytes:
    host, port = _target_for(request)
    with socket.create_connection((host, port), timeout=180) as s:
        s.sendall(request)
        s.shutdown(socket.SHUT_WR)
        chunks = []
        s.settimeout(180)
        while True:
            try:
                data = s.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks)


def _reader(fd: int, events: "queue.Queue[tuple]") -> None:
    buffer = bytearray()
    cat_active = False
    cat_parts: list[bytes] = []
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
            if line == b"@@CATBEG":
                cat_active = True
                cat_parts = []
                continue
            if line == b"@@CATEND":
                if cat_active:
                    events.put(("CAT", b"".join(cat_parts)))
                cat_active = False
                continue
            if cat_active:
                cat_parts.append(bytes(
                    c for c in line if c in
                    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="))
                continue
            marker_vf = line.find(b"@@VF ")
            marker_vc = line.find(b"@@VC ")
            if marker_vf >= 0:
                parts = line[marker_vf + 5:].split()
                try:
                    events.put(("H", int(parts[0]), int(parts[1])))
                except (ValueError, IndexError):
                    pass
            elif marker_vc >= 0:
                # 控制台可能把别的日志拼在行首，只取标记之后并按字段关键字解析。
                parts = line[marker_vc + 5:].split(b" ")
                try:
                    xid = int(parts[0])
                    seq = int(parts[1])
                    crc = int(parts[2], 16)
                    payload = b"".join(parts[3:])
                except (ValueError, IndexError):
                    continue
                payload = bytes(c for c in payload if c in
                                b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                b"abcdefghijklmnopqrstuvwxyz0123456789+/=")
                if crc16(payload) != crc:
                    continue
                events.put(("C", xid, seq, payload))
            elif _ECHO and line:
                sys.stdout.write(raw.decode(errors="replace") + "\n")
                sys.stdout.flush()
        if len(buffer) > 8 * 1024 * 1024:
            buffer.clear()


def _write_nsh(fd: int, command: str) -> None:
    for ch in command:
        os.write(fd, ch.encode("ascii"))
        time.sleep(0.001)
    os.write(fd, b"\n")
    time.sleep(0.05)


def _push_b64(fd: int, b64: bytes, remote: str) -> None:
    """以 base64 字面量 echo 追加写入，命令数与耗时都远小于 hex-printf。"""
    _write_nsh(fd, f"rm -f {remote}")
    text = b64.decode("ascii")
    for i in range(0, len(text), 50):
        _write_nsh(fd, f"echo '{text[i:i + 50]}' >> {remote}")


def _read_remote(fd: int, events: "queue.Queue[tuple]", remote: str,
                 timeout: float = 3.0) -> bytes:
    """回读远端文件内容（base64 行之间由 reader 以 @@CATBEG/@@CATEND 圈定）。"""
    _write_nsh(fd, "echo @@CATBEG")
    _write_nsh(fd, f"cat {remote}")
    _write_nsh(fd, "echo @@CATEND")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            event = events.get(timeout=0.2)
        except queue.Empty:
            continue
        if event[0] == "CAT":
            return event[1]
    return b""


def _push_b64_verified(fd: int, events: "queue.Queue[tuple]", b64: bytes,
                       remote: str) -> bool:
    """写入后回读比对；NSH 写坏时重推，最多 3 轮。"""
    text = b64.decode("ascii")
    for _ in range(3):
        _push_b64(fd, b64, remote)
        got = b"".join(_read_remote(fd, events, remote).split()).decode(
            "ascii", "ignore")
        if got == text:
            return True
        sys.stderr.write("[relay] 回读不一致，重推响应\n")
        sys.stderr.flush()
    return False


def main() -> int:
    fd = serial_push._open_raw_tty(PORT)
    events: "queue.Queue[tuple]" = queue.Queue()
    threading.Thread(target=_reader, args=(fd, events), daemon=True).start()
    sys.stderr.write(
        f"[relay] 已就绪：{PORT} -> proxy {PROXY_HOST}:{PROXY_PORT} / "
        f"llm {FORWARD_HOST}:{FORWARD_PORT}\n")
    sys.stderr.flush()

    # HMAC 需要时间；隧道不搬 NTP，直接注入开发机时间。
    if os.environ.get("SET_TIME", "1") == "1":
        time.sleep(float(os.environ.get("SET_TIME_DELAY", "6")))
        _write_nsh(fd, f"velaops set-time {int(time.time())}")

    if os.environ.get("AUTO_DIAG", "0") == "1":
        time.sleep(float(os.environ.get("AUTO_DIAG_DELAY", "80")))
        _write_nsh(fd, "echo x > /tmp/velaops-diagnose")
        sys.stderr.write("[relay] 已触发板端单轮 LLM 诊断\n")

    pending: dict[int, dict] = {}
    while True:
        try:
            event = events.get(timeout=0.1)
        except queue.Empty:
            event = None

        if event is not None:
            if event[0] == "H":
                _, xid, nchunks = event
                pending[xid] = {"n": nchunks, "chunks": {},
                                "deadline": time.monotonic() + ROUND_SECONDS,
                                "round": 0}
            elif event[0] == "C":
                _, xid, seq, payload = event
                entry = pending.setdefault(
                    xid, {"n": None, "chunks": {}, "deadline": time.monotonic() + ROUND_SECONDS,
                          "round": 0})
                entry["chunks"][seq] = payload

        now = time.monotonic()
        for xid, entry in list(pending.items()):
            total = entry["n"] if entry["n"] is not None else (
                max(entry["chunks"]) + 1 if entry["chunks"] else 0)
            if total <= 0 or now < entry["deadline"]:
                continue
            missing = [seq for seq in range(total) if seq not in entry["chunks"]]
            if not missing:
                data = b"".join(entry["chunks"][seq] for seq in range(total))
                del pending[xid]
                _write_nsh(fd, f"echo x > {ACK_REMOTE}")
                try:
                    request = base64.b64decode(data)
                except Exception as exc:  # noqa: BLE001
                    sys.stderr.write(f"[relay] base64 解码失败: {exc}\n")
                    _write_nsh(fd, f"echo 0 > {NACK_REMOTE}")
                    continue
                target = _target_for(request)
                first_line = request.split(b"\r\n", 1)[0]
                sys.stderr.write(
                    f"[relay] 收妥 {len(request)}B {first_line!r} -> {target}\n")
                sys.stderr.flush()
                try:
                    response = _forward(request)
                except Exception as exc:  # noqa: BLE001
                    sys.stderr.write(f"[relay] 转发失败: {exc}\n")
                    response = b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"
                sys.stderr.write(
                    f"[relay] 响应 {len(response)}B 首行 {response.split(b'\r\n', 1)[0]!r}\n")
                if os.environ.get("RELAY_DUMP", "0") == "1":
                    sys.stderr.write(
                        "[relay] 响应体: " + repr(response[:1600]) + "\n")
                sys.stderr.flush()
                # 直接回写：`cat` 回显会污染回读比对，误判反而拦截正常响应。
                _push_b64(fd, base64.b64encode(response), B64_REMOTE)
                _write_nsh(fd, f"echo x > {READY_REMOTE}")
                sys.stderr.write(f"[relay] 已回写响应 {len(response)} 字节\n")
                sys.stderr.flush()
            else:
                entry["round"] += 1
                if entry["round"] > MAX_ROUNDS:
                    sys.stderr.write(f"[relay] xid={xid} 重传超限，放弃\n")
                    del pending[xid]
                    continue
                _write_nsh(fd, f"echo {','.join(str(s) for s in missing)} > {NACK_REMOTE}")
                entry["deadline"] = now + ROUND_SECONDS
                sys.stderr.write(f"[relay] xid={xid} 缺 {len(missing)} 块，回 NACK\n")
                sys.stderr.flush()


if __name__ == "__main__":
    raise SystemExit(main())
