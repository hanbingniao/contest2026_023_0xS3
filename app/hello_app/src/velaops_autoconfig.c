/****************************************************************************
 * VelaOps TF 卡凭据自动配置实现。
 *
 * 凭据文件放在 TF 卡根目录 /velaops-credentials.txt，格式为
 * KEY=VALUE 文本；上电后由 bringup 拉起的 `velaops autoconfig` 任务
 * 读取该文件并自动完成：挂载 → 连 WiFi → 写设备配置 → 认证 →
 * 写 Agent LLM 路由 → 启动 ai_agent。
 ****************************************************************************/

#include "velaops_autoconfig.h"
#include "velaops_device_config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static void velaops_autoconfig_copy_field(char *output, size_t capacity,
                                          const char *value, size_t length)
{
  if (length >= capacity)
    {
      length = capacity - 1;
    }

  memcpy(output, value, length);
  output[length] = '\0';
}

static void velaops_autoconfig_trim(char **start, char **end)
{
  while (*start < *end && isspace((unsigned char)**start))
    {
      (*start)++;
    }

  while (*end > *start && isspace((unsigned char)(*end)[-1]))
    {
      (*end)--;
    }
}

int velaops_autoconfig_parse_credentials(const char *content,
                                         velaops_credentials_t *credentials)
{
  const char *cursor;

  if (content == NULL || credentials == NULL)
    {
      return -1;
    }

  memset(credentials, 0, sizeof(*credentials));

  cursor = content;
  while (*cursor != '\0')
    {
      const char *line_end = strchr(cursor, '\n');
      char *line_start;
      char *key;
      char *key_end;
      char *value;
      char *value_end;
      size_t key_length;
      size_t value_length;
      char line[160];
      size_t line_length;

      if (line_end == NULL)
        {
          line_end = cursor + strlen(cursor);
        }

      line_length = (size_t)(line_end - cursor);
      if (line_length > sizeof(line) - 1)
        {
          line_length = sizeof(line) - 1;
        }

      memcpy(line, cursor, line_length);
      line[line_length] = '\0';
      cursor = (*line_end == '\0') ? line_end : line_end + 1;

      line_start = line;
      key = line_start;
      key_end = line + line_length;
      velaops_autoconfig_trim(&key, &key_end);
      if (key == key_end || *key == '#')
        {
          continue;
        }

      value = memchr(key, '=', (size_t)(key_end - key));
      if (value == NULL)
        {
          continue;
        }

      /* KEY 部分去掉 '=' 与两侧空白；VALUE 取 '=' 之后到行尾。 */
      key_end = value;
      velaops_autoconfig_trim(&key, &key_end);
      value++;
      value_end = line + line_length;
      velaops_autoconfig_trim(&value, &value_end);

      key_length = (size_t)(key_end - key);
      value_length = (size_t)(value_end - value);
      if (key_length == 0 || value_length == 0)
        {
          continue;
        }

      if (key_length == 9 && strncmp(key, "WIFI_SSID", 9) == 0)
        {
          velaops_autoconfig_copy_field(credentials->wifi_ssid,
                                        sizeof(credentials->wifi_ssid),
                                        value, value_length);
        }
      else if (key_length == 13 &&
               strncmp(key, "WIFI_PASSWORD", 13) == 0)
        {
          velaops_autoconfig_copy_field(credentials->wifi_password,
                                        sizeof(credentials->wifi_password),
                                        value, value_length);
        }
      else if (key_length == 12 && strncmp(key, "MIMO_API_KEY", 12) == 0)
        {
          velaops_autoconfig_copy_field(credentials->api_key,
                                        sizeof(credentials->api_key),
                                        value, value_length);
          credentials->has_api_key = 1;
        }
      else if (key_length == 9 && strncmp(key, "DEMO_HOST", 9) == 0)
        {
          velaops_autoconfig_copy_field(credentials->demo_host,
                                        sizeof(credentials->demo_host),
                                        value, value_length);
          credentials->has_demo_host = 1;
        }
      else if (key_length == 9 && strncmp(key, "DEMO_PORT", 9) == 0)
        {
          velaops_autoconfig_copy_field(credentials->demo_port,
                                        sizeof(credentials->demo_port),
                                        value, value_length);
        }
      else if (key_length == 13 && strncmp(key, "DEVICE_SECRET", 13) == 0)
        {
          velaops_autoconfig_copy_field(credentials->device_secret,
                                        sizeof(credentials->device_secret),
                                        value, value_length);
          credentials->has_device_secret = 1;
        }
    }

  /* KEY 已在长度与内容双重比对后写入，无值视为未配置。 */
  if (credentials->wifi_ssid[0] == '\0' ||
      credentials->wifi_password[0] == '\0')
    {
      return -1;
    }

  return 0;
}

#ifdef __NuttX__

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nuttx/kmalloc.h>
#include <nuttx/sched.h>

#include "velaops_autoconfig.h"

#define VELAOPS_AUTOCONFIG_CRED_PATH "/mnt/sd/velaops-credentials.txt"
#define VELAOPS_AUTOCONFIG_MOUNT "/mnt/sd"
#define VELAOPS_AUTOCONFIG_DEFAULT_HOST "192.168.31.139"
#define VELAOPS_AUTOCONFIG_DEFAULT_PORT "28790"
#define VELAOPS_AUTOCONFIG_DEFAULT_SECRET \
  "a9c69d693a1cecd7f78d296c8ada2ca5"

int velaops_spawn_with_stack(int (*entry)(int, char **), const char *name,
                             char *const argv[], size_t stacksize)
{
  posix_spawnattr_t attr;
  FAR struct tcb_s *tcb;
  int pid;
  int ret;

  /* CONFIG_POSIX_SPAWN_DEFAULT_STACKSIZE 只有 2KB，task_spawn 直传入口
   * 会用默认小栈建任务，跑不了 mbedtls/printf，还会溢出破坏堆。
   * 这里仿照 nxtask_spawn_create 手写，显式指定栈大小。 */
  tcb = kmm_zalloc(sizeof(struct tcb_s) + sizeof(struct task_group_s));
  if (tcb == NULL)
    {
      return -ENOMEM;
    }

  atomic_set(&tcb->flags, TCB_FLAG_TTYPE_TASK | TCB_FLAG_FREE_TCB);

  ret = posix_spawnattr_init(&attr);
  if (ret == 0)
    {
      ret = posix_spawnattr_setstacksize(&attr, stacksize);
    }

  if (ret == 0)
    {
      ret = nxtask_init(tcb, name, entry, NULL, &attr, argv, NULL);
      if (ret < OK)
        {
          kmm_free(tcb);
        }
      else
        {
          pid = tcb->pid;
          nxtask_activate(tcb);
          ret = pid;
        }

      posix_spawnattr_destroy(&attr);
    }
  else
    {
      kmm_free(tcb);
      ret = -ret;
    }

  return ret;
}

static int velaops_autoconfig_mount_tf(void)
{
  static const char *devices[] = {"/dev/mmcsd0", "/dev/mmcsd1"};
  int attempt;
  size_t index;

  mkdir(VELAOPS_AUTOCONFIG_MOUNT, 0755);

  /* 手动重跑时卡可能已挂载：mount 会 EBUSY，但凭据文件可读即可继续。 */
  {
    FILE *probe = fopen(VELAOPS_AUTOCONFIG_CRED_PATH, "r");

    if (probe != NULL)
      {
        fclose(probe);
        printf("velaops autoconfig: TF 已处于挂载状态\n");
        return 0;
      }
  }

  for (attempt = 0; attempt < 6; attempt++)
    {
      for (index = 0; index < 2; index++)
        {
          if (mount(devices[index], VELAOPS_AUTOCONFIG_MOUNT,
                    "vfat", 0, NULL) == 0)
            {
              printf("velaops autoconfig: TF 已挂载 %s\n", devices[index]);
              return 0;
            }
        }

      sleep(2);
    }

  return -ENOENT;
}

static int velaops_autoconfig_read_credentials(velaops_credentials_t *cred)
{
  char *buffer;
  long size;
  FILE *file;
  int result = -1;

  file = fopen(VELAOPS_AUTOCONFIG_CRED_PATH, "r");
  if (file == NULL)
    {
      printf("velaops autoconfig: 未找到 %s (errno=%d)\n",
             VELAOPS_AUTOCONFIG_CRED_PATH, errno);
      return -1;
    }

  fseek(file, 0, SEEK_END);
  size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size <= 0 || size > 4096)
    {
      fclose(file);
      return -1;
    }

  buffer = malloc((size_t)size + 1);
  if (buffer != NULL)
    {
      size_t read_count = fread(buffer, 1, (size_t)size, file);
      buffer[read_count] = '\0';
      result = velaops_autoconfig_parse_credentials(buffer, cred);
      free(buffer);
    }

  fclose(file);
  return result;
}

static int velaops_autoconfig_connect_wifi(const velaops_credentials_t *cred)
{
  char command[160];
  int attempt;

  /* 上电早期 WiFi 固件/协议栈还在初始化，过早 ioctl 会直接打挂 wifi
   * 内核线程（实测 EXCCAUSE=14 空指针）。先只读探测 wlan0 出现，
   * 再等几秒让协议栈稳定，全程不调 wapi。 */
  for (attempt = 0; attempt < 20; attempt++)
    {
      FILE *ifconfig = popen("ifconfig wlan0", "r");
      int present = 0;

      if (ifconfig != NULL)
        {
          char line[256];

          while (fgets(line, sizeof(line), ifconfig) != NULL)
            {
              if (strstr(line, "wlan0") != NULL ||
                  strstr(line, "Flags") != NULL ||
                  strstr(line, "inet") != NULL)
                {
                  present = 1;
                  break;
                }
            }

          pclose(ifconfig);
        }

      if (present)
        {
          break;
        }

      printf("velaops autoconfig: 等待 wlan0 出现 (%d/20)\n", attempt + 1);
      sleep(3);
    }

  sleep(5);

  /* wapi 失败可重试；单次失败不致命，最终成败以拿到 IP 为准。 */
  for (attempt = 0; attempt < 5; attempt++)
    {
      snprintf(command, sizeof(command), "wapi mode wlan0 2");
      if (system(command) == 0)
        {
          break;
        }

      printf("velaops autoconfig: wapi mode 重试 (%d/5)\n", attempt + 1);
      sleep(3);
    }

  snprintf(command, sizeof(command), "wapi psk wlan0 '%s' 3 2",
           cred->wifi_password);
  system(command);
  snprintf(command, sizeof(command), "wapi essid wlan0 '%s' 1",
           cred->wifi_ssid);
  system(command);

  for (attempt = 0; attempt < 15; attempt++)
    {
      char line[256];
      FILE *ifconfig;

      system("renew wlan0");

      /* 多轮拿不到 IP 时怀疑驱动假活（RUNNING 但收发全停），
       * 复位接口并重新关联，而不是干等。 */
      if (attempt > 0 && attempt % 3 == 0)
        {
          system("ifdown wlan0");
          system("ifup wlan0");
          snprintf(command, sizeof(command), "wapi psk wlan0 '%s' 3 2",
                   cred->wifi_password);
          system(command);
          snprintf(command, sizeof(command), "wapi essid wlan0 '%s' 1",
                   cred->wifi_ssid);
          system(command);
        }

      sleep(3);

      /* 拿到 wlan0 的 IPv4 地址即视为联网成功。 */
      ifconfig = popen("ifconfig wlan0", "r");
      if (ifconfig == NULL)
        {
          continue;
        }

      while (fgets(line, sizeof(line), ifconfig) != NULL)
        {
          if (strstr(line, "inet ") != NULL &&
              strstr(line, "0.0.0.0") == NULL)
            {
              pclose(ifconfig);
              printf("velaops autoconfig: WiFi 已联网: %s", line);
              return 0;
            }
        }

      pclose(ifconfig);
    }

  return -1;
}

static int velaops_autoconfig_write_device_config(
    const velaops_credentials_t *cred)
{
  char path[64];
  FILE *file;
  velaops_device_config_t existing;

  /* 烧录后若 /data 仍保留有效配对配置，绝不覆盖它。覆盖会把 Proxy
   * 的随机密钥替换成旧默认值，导致 WiFi 正常但认证失败。 */
  if (velaops_device_config_load(VELAOPS_CONFIG_FILE, &existing) ==
      VELAOPS_CONFIG_OK)
    {
      velaops_device_config_clear(&existing);
      printf("velaops autoconfig: 保留已有配对设备配置\n");
      return 0;
    }

  mkdir("/data", 0755);
  mkdir("/data/velaops", 0755);
  snprintf(path, sizeof(path), "/data/velaops/config.json");
  file = fopen(path, "w");
  if (file == NULL)
    {
      return -1;
    }

  if (!cred->has_device_secret)
    {
      printf("velaops autoconfig: TF 缺少 DEVICE_SECRET，拒绝写入默认密钥\n");
      return -1;
    }
  fprintf(file,
          "{\"schema_version\":1,\"host\":\"%s\",\"port\":\"%s\","
          "\"device_id\":\"eye-001\",\"secret\":\"%s\"}",
          cred->has_demo_host ? cred->demo_host
                              : VELAOPS_AUTOCONFIG_DEFAULT_HOST,
          cred->demo_port[0] != '\0' ? cred->demo_port
                                     : VELAOPS_AUTOCONFIG_DEFAULT_PORT,
          cred->device_secret);
  fclose(file);
  return 0;
}

static int velaops_autoconfig_write_agent_router(
    const velaops_credentials_t *cred)
{
  FILE *file;

  if (!cred->has_api_key)
    {
      printf("velaops autoconfig: TF 未提供 MIMO_API_KEY，跳过路由预写\n");
      return 0;
    }

  /* ai_agent 的 cfgstore 以扁平 JSON 存储，路由后端是字符串化的 JSON，
   * 与 CLI `router_set mimo` 落盘格式一致，可免去串口喂命令。
   * 设备直连外网 TLS 在新场地不稳定，改走开发机上的 LLM 转发器
   * （docs/tools/llm_forwarder.py，明文 HTTP），主机代劳 TLS。 */
  mkdir("/data/ai_agent", 0755);
  mkdir("/data/ai_agent/config", 0755);
  file = fopen("/data/ai_agent/config/config.json", "w");
  if (file == NULL)
    {
      return -1;
    }

  fprintf(file,
          "{\"llm_backend_0\":\"{\\\"host\\\":\\\"%s\\\","
          "\\\"path\\\":\\\"/v1/chat/completions\\\",\\\"port\\\":"
          "\\\"%s\\\",\\\"api_key\\\":\\\"%s\\\",\\\"model\\\":"
          "\\\"mimo-v2.5\\\",\\\"priority\\\":0,\\\"cost_tier\\\":1}\"}",
          cred->has_demo_host && cred->demo_host[0] != '\0'
              ? cred->demo_host
              : "api.xiaomimimo.com",
          cred->has_demo_host && cred->demo_host[0] != '\0' ? "28792" : "443",
          cred->api_key);
  fclose(file);
  printf("velaops autoconfig: Agent 路由已预写 mimo\n");
  return 0;
}

static void velaops_autoconfig_install_demo_skill(void)
{
  FILE *source;
  FILE *target;
  char buffer[512];
  size_t count;

  /* /data 是 tmpfs，重启后清空；把随凭据卡提供的 Skill 每次启动复制
   * 到 Agent 目录，保证演示机无需串口再次下发文件。 */
  source = fopen("/mnt/sd/server-incident-response.md", "r");
  if (source == NULL)
    {
      printf("velaops autoconfig: TF 未提供事件响应 Skill，使用内置 Skills\n");
      return;
    }

  mkdir("/data/ai_agent", 0755);
  mkdir("/data/ai_agent/skills", 0755);
  target = fopen("/data/ai_agent/skills/server-incident-response.md", "w");
  if (target == NULL)
    {
      fclose(source);
      printf("velaops autoconfig: 事件响应 Skill 写入失败\n");
      return;
    }

  while ((count = fread(buffer, 1, sizeof(buffer), source)) > 0)
    {
      if (fwrite(buffer, 1, count, target) != count)
        {
          printf("velaops autoconfig: 事件响应 Skill 写入中断\n");
          break;
        }
    }

  fclose(target);
  fclose(source);
  printf("velaops autoconfig: 事件响应 Skill 已加载\n");
}

int velaops_autoconfig_run(int verbose)
{
  velaops_credentials_t credentials;
  int status;

  (void)verbose;
  printf("velaops autoconfig: 开始（等待驱动就绪）\n");
  sleep(5);

  status = velaops_autoconfig_mount_tf();
  if (status != 0)
    {
      printf("velaops autoconfig: TF 挂载失败，退出自动配置（可手动配置）\n");
      return status;
    }

  status = velaops_autoconfig_read_credentials(&credentials);
  if (status != 0)
    {
      printf("velaops autoconfig: 凭据文件缺失或格式错误，退出\n");
      return status;
    }

  printf("velaops autoconfig: 凭据读取成功，开始连接 WiFi %s\n",
         credentials.wifi_ssid);
  status = velaops_autoconfig_connect_wifi(&credentials);
  if (status != 0)
    {
      printf("velaops autoconfig: WiFi 连接失败，退出自动配置（可手动配置）\n");
      return status;
    }

  if (velaops_autoconfig_write_device_config(&credentials) != 0)
    {
      printf("velaops autoconfig: 设备配置写入失败，退出自动配置（可手动配置）\n");
      return -1;
    }

  /* 冷启动后链路偶发假活掉线，首次认证可能失败；重试最多 10 分钟，
   * 避免一次网络抖动就让整条自启链路（agent + 看板）永远不拉起。 */
  {
    int auth_attempt;
    int auth_status = -1;

    for (auth_attempt = 0; auth_attempt < 40; auth_attempt++)
      {
        if (system("velaops auth-check") == 0)
          {
            auth_status = 0;
            break;
          }

        printf("velaops autoconfig: 认证失败，15s 后重试 (%d/40)\n",
               auth_attempt + 1);
        system("renew wlan0");
        sleep(15);
      }

    if (auth_status != 0)
      {
        printf("velaops autoconfig: 认证持续失败，退出自动配置（可手动配置）\n");
        return -1;
      }
  }

  if (velaops_autoconfig_write_agent_router(&credentials) != 0)
    {
      printf("velaops autoconfig: Agent 路由预写失败，退出自动配置（可手动配置）\n");
      return -1;
    }

  velaops_autoconfig_install_demo_skill();

  /* Provider 必须在 ai_agent 主循环启动前注册。若先启动 Agent，
   * 上电自动配置虽然能完成联网和路由写入，但自然语言请求看不到
   * 队伍的 VelaOps 工具。通过固定无参数命令复用同一注册入口，避免
   * 在自动配置中复制工具回调和白名单逻辑。 */
  if (system("velaops agent-install") != 0)
    {
      printf("velaops autoconfig: VelaOps 工具注册失败，退出自动配置\n");
      return -1;
    }

  /* CONFIG_LIBC_EXECFUNCS 未开启，按名查找不可用；直传入口又只用 2KB
   * 默认栈，这里统一走显式指定栈的启动入口。 */
  {
    extern int ai_agent_main(int argc, char *argv[]);

    status = velaops_spawn_with_stack(ai_agent_main, "ai_agent", NULL,
                                      65536);
  }

  if (status < 0)
    {
      printf("velaops autoconfig: ai_agent 启动失败 (%d)\n", status);
      return -1;
    }

  printf("velaops autoconfig: 全部完成，ai_agent 已启动\n");

  /* 屏显看板（5s 资源监测）是常驻任务，不拉起则屏幕保持白屏。
   * 栈给 32KB；argv[0] 由内核自填，只传子命令。看板自带网络自愈：
   * 连续巡检失败会自动续租/重新关联，/tmp/velaops-no-monitor 可临时关。 */
  {
    extern int velaops_main(int argc, char *argv[]);
    static char *const velaops_monitor_argv[] = { "monitor", NULL };
    int monitor_pid;

    if (access("/tmp/velaops-no-monitor", F_OK) == 0)
      {
        printf("velaops autoconfig: /tmp/velaops-no-monitor 存在，跳过看板\n");
      }
    else
      {
        monitor_pid = velaops_spawn_with_stack(velaops_main, "velaops-mon",
                                               velaops_monitor_argv, 32768);
        if (monitor_pid < 0)
          {
            printf("velaops autoconfig: 屏显看板启动失败 (%d)\n",
                   monitor_pid);
          }
        else
          {
            printf("velaops autoconfig: 屏显看板已启动 pid=%d\n",
                   monitor_pid);
          }
      }
  }

  return 0;
}

#endif /* __NuttX__ */
