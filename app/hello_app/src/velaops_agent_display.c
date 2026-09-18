/****************************************************************************
 * VelaOps Agent 回发内容 → LCD 告警框桥接。
 *
 * ai_agent 在 outbound 回发处调用弱符号 velaops_notify_agent_reply；本文件
 * 提供设备侧强实现：
 *   1) 诊断结论（含 schema_version/status 的 JSON）到达时，写 /tmp/velaops-llm-done
 *      通知看板"单轮诊断已结束，可恢复巡检"；
 *   2) 把结论里的 recommended_action 映射成 <=16 个可打印 ASCII 短语写入
 *      /tmp/velaops-popup.txt，由看板用闪烁告警框展示。
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "cJSON.h"

#define VELAOPS_AGENT_POPUP_PATH "/tmp/velaops-popup.txt"
#define VELAOPS_AGENT_DONE_PATH "/tmp/velaops-llm-done"
#define VELAOPS_AGENT_DISPLAY_TEXT_MAX 16

static void velaops_agent_upper(char *text)
{
  size_t index;

  for (index = 0; text[index] != '\0'; index++)
    {
      if (text[index] >= 'a' && text[index] <= 'z')
        {
          text[index] = (char)(text[index] - 'a' + 'A');
        }
    }
}

/* 从可能带 markdown 围栏的回复中截出 JSON 主体。 */
static const char *velaops_agent_json_body(const char *content)
{
  const char *begin = strchr(content, '{');

  return begin;
}

static void velaops_agent_map_display_text(const char *content, char *output,
                                           size_t capacity)
{
  const char *body = velaops_agent_json_body(content);
  cJSON *root;
  const cJSON *recommendation;
  const cJSON *action;
  const cJSON *target;
  const cJSON *status;
  char label[24];

  if (body == NULL)
    {
      return;
    }
  root = cJSON_Parse(body);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return;
    }

  recommendation = cJSON_GetObjectItemCaseSensitive(root,
                                                    "recommended_action");
  action = cJSON_IsObject(recommendation) ?
           cJSON_GetObjectItemCaseSensitive(recommendation, "action") : NULL;
  target = cJSON_IsObject(recommendation) ?
           cJSON_GetObjectItemCaseSensitive(recommendation, "target") : NULL;
  status = cJSON_GetObjectItemCaseSensitive(root, "status");

  label[0] = '\0';
  if (cJSON_IsString(action) && action->valuestring != NULL)
    {
      if (strcmp(action->valuestring, "restart_service") == 0)
        {
          const char *name = (cJSON_IsString(target) &&
                              target->valuestring != NULL) ?
                             target->valuestring : "SERVICE";

          snprintf(label, sizeof(label), "RESTART %.11s", name);
        }
      else if (strcmp(action->valuestring, "retry_check") == 0)
        {
          snprintf(label, sizeof(label), "RETRY CHECK");
        }
      else if (strcmp(action->valuestring, "none") != 0)
        {
          snprintf(label, sizeof(label), "%.15s", action->valuestring);
        }
    }
  else if (cJSON_IsString(status) && status->valuestring != NULL)
    {
      /* 只在确有异常时覆盖告警框，避免模型误判 normal 把本地告警刷掉。 */
      if (strcmp(status->valuestring, "critical") == 0)
        {
          snprintf(label, sizeof(label), "CRITICAL");
        }
      else if (strcmp(status->valuestring, "warning") == 0)
        {
          snprintf(label, sizeof(label), "WARNING");
        }
    }

  cJSON_Delete(root);
  if (label[0] == '\0')
    {
      return;
    }
  velaops_agent_upper(label);
  snprintf(output, capacity, "%.*s", VELAOPS_AGENT_DISPLAY_TEXT_MAX, label);
}

/* 弱符号强实现：见 packages/ai_agent/src/agent_main.c。 */
void velaops_notify_agent_reply(const char *channel, const char *content)
{
  char text[VELAOPS_AGENT_DISPLAY_TEXT_MAX + 1];
  FILE *file;

  if (content == NULL || channel == NULL || strcmp(channel, "cli") != 0)
    {
      return;
    }
  if (strstr(content, "\"schema_version\"") == NULL ||
      strstr(content, "\"status\"") == NULL)
    {
      return;
    }

  file = fopen(VELAOPS_AGENT_DONE_PATH, "w");
  if (file != NULL)
    {
      fputs("done", file);
      fclose(file);
    }

  text[0] = '\0';
  velaops_agent_map_display_text(content, text, sizeof(text));
  syslog(LOG_ERR, "velaops: agent 诊断回发 -> 告警框 [%s]\n",
         text[0] != '\0' ? text : "（本地摘要保留）");
  if (text[0] == '\0')
    {
      return;
    }
  file = fopen(VELAOPS_AGENT_POPUP_PATH, "w");
  if (file != NULL)
    {
      fwrite(text, 1, strlen(text), file);
      fclose(file);
    }
}
