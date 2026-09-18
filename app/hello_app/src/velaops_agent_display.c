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
  const cJSON *display;
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
  display = cJSON_GetObjectItemCaseSensitive(root, "display");

  {
    const char *state = (cJSON_IsString(status) &&
                         status->valuestring != NULL) ?
                        status->valuestring : "";
    int alarm = (strcmp(state, "warning") == 0 ||
                 strcmp(state, "critical") == 0);
    int has_action = cJSON_IsString(action) && action->valuestring != NULL &&
                     strcmp(action->valuestring, "none") != 0;

    label[0] = '\0';

    /* 优先使用 LLM 自己给的短结论（display）：只在确有异常/建议动作时采用，
     * 避免模型误判 normal 时把本地告警刷掉。只保留可打印 ASCII。 */
    if ((alarm || has_action) && cJSON_IsString(display) &&
        display->valuestring != NULL)
      {
        const unsigned char *cursor =
            (const unsigned char *)display->valuestring;
        size_t out = 0;

        while (*cursor != '\0' && out < sizeof(label) - 1)
          {
            if (*cursor >= 0x20 && *cursor <= 0x7e)
              {
                label[out++] = (char)*cursor;
              }
            cursor++;
          }
        label[out] = '\0';
      }

    if (label[0] == '\0' && has_action)
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
        else
          {
            snprintf(label, sizeof(label), "%.15s", action->valuestring);
          }
      }

    /* 无动作建议时，按 evidence 给出更直观的短语；只在确有异常时覆盖，
     * 避免模型误判 normal 把本地告警刷掉。 */
    if (label[0] == '\0' && alarm)
      {
        const cJSON *evidence = cJSON_GetObjectItemCaseSensitive(root,
                                                                 "evidence");
        const cJSON *item;

        cJSON_ArrayForEach(item, evidence)
          {
            const cJSON *metric = cJSON_GetObjectItemCaseSensitive(item,
                                                                   "metric");

            if (!cJSON_IsString(metric) || metric->valuestring == NULL)
              {
                continue;
              }
            if (strstr(metric->valuestring, "cpu") != NULL)
              {
                snprintf(label, sizeof(label), "CPU HIGH");
                break;
              }
            if (strstr(metric->valuestring, "disk") != NULL)
              {
                snprintf(label, sizeof(label), "DISK HIGH");
                break;
              }
            if (strstr(metric->valuestring, "memory") != NULL)
              {
                snprintf(label, sizeof(label), "MEM HIGH");
                break;
              }
            if (strstr(metric->valuestring, "service") != NULL)
              {
                snprintf(label, sizeof(label), "SERVICE DOWN");
                break;
              }
            if (strstr(metric->valuestring, "port") != NULL)
              {
                snprintf(label, sizeof(label), "PORT CLOSED");
                break;
              }
          }
      }
    if (label[0] == '\0' && alarm)
      {
        snprintf(label, sizeof(label), "%s",
                 strcmp(state, "critical") == 0 ? "CRITICAL" : "WARNING");
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
