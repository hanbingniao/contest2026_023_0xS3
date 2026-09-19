# 交接提示词（复制给下一个会话）

> 用途：本会话上下文已满。把下面「提示词正文」整段复制给下一个会话作为起点。

---

## 提示词正文

你是 openvela AI 大赛 **ai_agent 赛道**队伍仓项目的开发助手。项目路径：

- 工作区：`/home/lu/桌面/openvela`
- 队伍仓（只改这里）：`/home/lu/桌面/openvela/contest2026_023_0xS3`，分支 `fix/network-stability`
- 构建：`bash contest2026_023_0xS3/docs/tools/apply_nuttx_patches.sh` 然后
  `./build.sh vendor/espressif/boards/esp32s3/esp32s3-eye/configs/openvela`
- 板子：ESP32-S3-EYE，USB CDC 控制台端口会在 `/dev/ttyACM0` 与 `ttyACM1` 间跳变

### 约束（务必遵守）
- **上游 0 改动**：不改 `nuttx/`、`vendor/`、`packages/ai_agent/` 源码；所有上游改动放
  `patches/{nuttx,vendor,ai_agent}/`，由 `apply_nuttx_patches.sh` 编译前注入（可重复执行）。
- 代码注释用中文；**不要主动 git commit**，除非我明确要求。
- LCD 是 5x7 ASCII 字库，**只能显示英文/数字**，中文无效。

### 当前稳定状态（2026-09-19，已提交 9e86b9d 及以前）
- 串口有线（`TRANSPORT=serial`）稳定：认证成功、巡检持续、`invalid_response/transport_error/panic` 基本为 0。
- 三页看板：概览/性能/运维（CPU/MEM/DISK 仪表、CPU 负载+曲线、服务/端口/磁盘）。
- CPU 阈值告警（≥85%）→ LCD 闪烁告警框 + 板载 LED（`/dev/userleds`）闪烁；
  本地摘要含元凶进程（Proxy 新增 `top_process`）。
- **LLM 上屏已打通**：ai_agent outbound 钩子把诊断结论映射成短英文写入
  `/tmp/velaops-popup.txt`，看板弹窗更新（如 `CPU HIGH`）。
- 开机自启 + `VELAOPS/BOOTING` 开机画面 + 连接状态页（`CONNECTION / SERIAL WAIT RELAY`）。
- 性能压测工具：`docs/tools/stress_cpu.py`（压满开发机 CPU 触发告警）。

### 运行环境（开发机侧）
- 四个 user-systemd 服务：`velaops-proxy`(127.0.0.1:28790)、`velaops-llm-forwarder`
  (127.0.0.1:28792)、`velaops-lan-ntp`(40123)、`velaops-demo-target`(127.0.0.1:28791，
  状态页 `docs/tools/demo_target_app.py`，`/status` 返回 PID/uptime/requests)。
- relay 用 `systemd-run --user --unit=velaops-relay --collect -p Restart=always` 常驻：
  `PORT=/dev/ttyACM0 SET_TIME=1 PROXY_HOST=127.0.0.1 PROXY_PORT=28790 \
   FORWARD_HOST=127.0.0.1 FORWARD_PORT=28792 python3 docs/tools/serial_llm_relay.py`。
  relay 自动探测 `ttyACM*` 并断线重连；`RELAY_INJECT_FILE=<file>` 可在开机稳定后注入一条
  NSH 命令（联调用）。
- 板端凭据卡 `/mnt/sd/velaops-credentials.txt`（`TRANSPORT=serial|wifi` 一键切换）。
  用 `docs/tools/serial_push.py <local> <remote>` 下发；推送时板端若刷屏会冲散首行，**必须回读校验**。

### 已知问题（不要再踩）
- **单条半双工隧道**：日志/回显与 base64 帧共用一路 CDC，高突发（如"实体批准修复"）会偶发
  `transport_error/no-ack`。已做：全局互斥、逐行加锁、丢弃过期 xid、90s 快速 504、
  auth 40s 重传容忍。**结论：不换独立 UART/多连接，修复执行难以 100% 稳**，
  故演示主线定为"告警→LLM 诊断"，批准只讲设计。
- **WiFi 不可用**：同网段可达也无用——板端 autoconfig 卡在"连接 WiFi"阶段，配置写不进去，
  看板 `io_error`。别再花时间在 WiFi。
- **实体批准**：按键交互已通（看板线程统一读按键、长按 2s 写 `/tmp/velaops-approve`、
  达标即消弹窗），但重启请求在隧道上不稳。板子有 6 个功能键（RST 不可配，另 5 个：MENU/
  UP+/DOWN/PLAY/BOOT），**NuttX 现仅注册 `BUTTON_BOOT`**。
- 重启/复位：esptool 的 `run/chip-id` 会把板子带进 download 模式；用 `write-flash ... --after
  hard-reset` 或**物理重新上电**最可靠。

### 本次要做的任务（按优先级）
1. **让 LLM 充分发挥作用并上屏（主线）**：
   - 把诊断 prompt 与 Skill 改为**英文**（LCD 仅 ASCII），要求 LLM 输出**英文** `display`
     短句（≤15 ASCII）解释"是什么进程/建议怎么做"。
   - 看板支持把 LLM 的**英文回复分页显示**（一屏放不下就分多屏）。
2. **按键扩展**：在板级按钮驱动补上 ESP32-S3-EYE 其余功能键（查原理图 GPIO；参考
   `vendor/espressif/boards/esp32s3/esp32s3-eye/src/esp32s3_buttons.c`，当前只有
   `BUTTON_BOOT`）。用 **UP/DOWN 翻页**看 LLM 回复；把**批准改到 PLAY/MENU**（解放 BOOT）。
   这属于板级改动 → 走 `patches/vendor`（或 `patches/nuttx`，看实际编译的是哪个副本）。
3. **Skill 审阅**：`app/hello_app/skills/server-incident-response.md`（26 行，含中文
   summary/evidence reason，输出契约见文末）→ 转英文并请我审阅（比赛要求形成 Skill）。
4. 结束前更新 `docs/PROJECT_MEMORY.md`、`docs/DEMO_RUNBOOK.md`，并在需要时提交。

### 关键文件
- 看板渲染：`app/hello_app/src/velaops_dashboard.c`（三页 + 弹窗渲染）
- 显示状态/接口：`app/hello_app/include/velaops_display.h`、`src/velaops_display.c`
- 主任务/看板/告警/LLM 入队/批准：`app/hello_app/velaops_main.c`
- 事件判定：`app/hello_app/src/velaops_resource_incident.c`、`velaops_local_diagnosis.c`
- 串口隧道：`app/hello_app/src/velaops_serial_tunnel.c`
- Agent 回发→LCD 桥接：`app/hello_app/src/velaops_agent_display.c`（钩子注册见
  `src/velaops_agent_tools.c`，上游钩子补丁在 `patches/ai_agent/`）
- 开发机 relay：`docs/tools/serial_llm_relay.py`
- 上游补丁：`patches/ai_agent/`、`patches/nuttx/`、`patches/vendor/`
- 交接文档：`docs/SERIAL_TRANSPORT.md`、`docs/PROJECT_MEMORY.md`（第 8、9 节）
- 压测：`docs/tools/stress_cpu.py`

### Skill 输出契约（需转英文）
```json
{"schema_version":1,"status":"normal|warning|critical|unknown",
 "summary":"<english short>",
 "evidence":[{"metric":"field","value":"observed","reason":"<english>"}],
 "root_cause_candidates":[],
 "recommended_action":{"action":"none|restart_service|retry_check","target":"alias or empty",
   "risk":"read_only|change","requires_physical_approval":true},
 "confidence":0.0,
 "display":"<==15 printable ASCII, e.g. CPU 99 PYTHON / RESTART PROXY / DISK FULL>"}
```

先只读代码与文档确认现状，再动手；每步小改并真机验证。
