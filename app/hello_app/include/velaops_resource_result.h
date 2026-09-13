/****************************************************************************
 * VelaOps Proxy 聚合资源结果适配器。
 ****************************************************************************/

#ifndef VELAOPS_RESOURCE_RESULT_H
#define VELAOPS_RESOURCE_RESULT_H

#include <stdbool.h>
#include "velaops_health.h"

typedef struct
{
  velaops_memory_observation_t memory;
  double disk_percent;
  bool service_active;
  bool port_reachable;
  int port_latency_ms;
} velaops_resource_observation_t;

int velaops_resource_result_parse(const char *result_json,
                                  velaops_resource_observation_t *result);

#endif /* VELAOPS_RESOURCE_RESULT_H */
