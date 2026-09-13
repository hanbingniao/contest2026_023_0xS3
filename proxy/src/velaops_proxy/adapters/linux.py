"""Linux 本机只读诊断适配器。"""

from __future__ import annotations

from pathlib import Path
import os
import shutil
import socket
import time

from ..diagnostics import (
    DiskSnapshot,
    LogSnapshot,
    MemorySnapshot,
    PortSnapshot,
    ServiceSnapshot,
)
from ..config import ServiceConfig
from ..executor import BoundedCommandExecutor, CommandResult, CommandSpec
from ..protocol import ErrorCode
from ..actions import ActionError
from ..redaction import redact_text


class LinuxSystemInspector:
    """仅使用固定绝对路径程序和配置中的白名单参数。"""

    def __init__(self, executor: BoundedCommandExecutor | None = None) -> None:
        self._executor = executor or BoundedCommandExecutor()

    def service_status(self, service: ServiceConfig) -> ServiceSnapshot:
        result = self._run(
            self._systemctl_argv(
                service.manager,
                "show",
                "--no-pager",
                "--property=LoadState,ActiveState,SubState,Result,ExecMainStatus",
                "--",
                service.unit,
            ),
            max_bytes=8192,
            user_manager=service.manager == "user",
        )
        fields = self._key_values(result.stdout)
        status_text = fields.get("ExecMainStatus", "")
        return ServiceSnapshot(
            load_state=fields.get("LoadState", "unknown"),
            active_state=fields.get("ActiveState", "unknown"),
            sub_state=fields.get("SubState", "unknown"),
            result=fields.get("Result", "unknown"),
            main_exit_status=int(status_text) if status_text.isdecimal() else None,
        )

    def check_port(self, host: str, port: int) -> PortSnapshot:
        started = time.monotonic()
        try:
            with socket.create_connection((host, port), timeout=1.0):
                reachable = True
        except OSError:
            reachable = False
        return PortSnapshot(
            reachable=reachable,
            latency_ms=max(0, int((time.monotonic() - started) * 1000)),
        )

    def disk_usage(self, path: str) -> DiskSnapshot:
        usage = shutil.disk_usage(path)
        used = usage.total - usage.free
        percent = 0.0 if usage.total == 0 else round(used * 100 / usage.total, 2)
        return DiskSnapshot(usage.total, used, usage.free, percent)

    def memory_usage(self) -> MemorySnapshot:
        fields = self._meminfo(Path("/proc/meminfo").read_text(encoding="ascii"))
        total = fields["MemTotal"] * 1024
        available = fields["MemAvailable"] * 1024
        used = total - available
        percent = 0.0 if total == 0 else round(used * 100 / total, 2)
        return MemorySnapshot(total, available, used, percent)

    def service_log(self, service: ServiceConfig, lines: int) -> LogSnapshot:
        unit_option = "--user-unit" if service.manager == "user" else "--unit"
        result = self._run(
            (
                "/usr/bin/journalctl",
                "--no-pager",
                "--quiet",
                "--output=short-iso",
                f"--lines={lines}",
                unit_option,
                service.unit,
            ),
            max_bytes=32 * 1024,
        )
        return LogSnapshot(redact_text(result.stdout), result.stdout_truncated)

    def restart_service(self, service: ServiceConfig) -> ServiceSnapshot:
        argv = self._systemctl_argv(
            service.manager,
            "restart",
            "--",
            service.unit,
        )
        if service.restart_via_sudo:
            argv = ("/usr/bin/sudo", "-n", "--", *argv)
        self._run(
            argv,
            max_bytes=8192,
            user_manager=service.manager == "user",
            timeout_seconds=20,
        )
        return self.service_status(service)

    def _run(
        self,
        argv: tuple[str, ...],
        max_bytes: int,
        user_manager: bool = False,
        timeout_seconds: float = 5,
    ) -> CommandResult:
        environment = None
        if user_manager:
            environment = {"XDG_RUNTIME_DIR": f"/run/user/{os.geteuid()}"}
        result = self._executor.run(
            CommandSpec(
                argv,
                timeout_seconds=timeout_seconds,
                max_stream_bytes=max_bytes,
                env=environment,
            )
        )
        if result.timed_out:
            raise ActionError(ErrorCode.EXECUTION_TIMEOUT, "系统诊断超时")
        if result.exit_code != 0:
            raise ActionError(ErrorCode.INTERNAL_ERROR, "系统诊断失败")
        return result

    @staticmethod
    def _systemctl_argv(manager: str, *arguments: str) -> tuple[str, ...]:
        if manager == "system":
            return ("/usr/bin/systemctl", *arguments)
        if manager == "user":
            return ("/usr/bin/systemctl", "--user", *arguments)
        raise ValueError("systemd manager 不合法")

    @staticmethod
    def _key_values(text: str) -> dict[str, str]:
        result: dict[str, str] = {}
        for line in text.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                result[key] = value
        return result

    @staticmethod
    def _meminfo(text: str) -> dict[str, int]:
        values: dict[str, int] = {}
        for line in text.splitlines():
            parts = line.replace(":", "").split()
            if len(parts) >= 2 and parts[0] in {"MemTotal", "MemAvailable"}:
                if not parts[1].isdecimal():
                    raise RuntimeError("/proc/meminfo 数值不合法")
                values[parts[0]] = int(parts[1])
        if set(values) != {"MemTotal", "MemAvailable"}:
            raise RuntimeError("/proc/meminfo 缺少内存字段")
        return values
