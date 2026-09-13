/****************************************************************************
 * VelaOps 本地规则诊断器。
 *
 * 在 LLM 不可用时，把只读资源证据转换为与 Agent Skill 对齐的结构化建议。
 * 本模块不访问网络、不执行 Action，只负责确定性判断和 JSON 编码。
 ****************************************************************************/

#ifndef VELAOPS_LOCAL_DIAGNOSIS_H
#define VELAOPS_LOCAL_DIAGNOSIS_H

#include <stddef.h>

typedef enum
{
  VELAOPS_DIAGNOSIS_UNKNOWN = 0,
  VELAOPS_DIAGNOSIS_NORMAL,
  VELAOPS_DIAGNOSIS_WARNING,
  VELAOPS_DIAGNOSIS_CRITICAL
} velaops_diagnosis_status_t;

int velaops_local_diagnosis_evaluate(
    const char *resource_json, velaops_diagnosis_status_t *status);
int velaops_local_diagnosis_build(const char *resource_json, char *output,
                                  size_t output_capacity);

#endif /* VELAOPS_LOCAL_DIAGNOSIS_H */
