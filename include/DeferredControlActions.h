#ifndef DEFERRED_CONTROL_ACTIONS_H
#define DEFERRED_CONTROL_ACTIONS_H

#include <stdint.h>
#include "SensorDataManager.h"
#include "RelayController.h"

bool enqueueLocalThresholdMutation(float tMin, float tMax, float hMin, float hMax, uint32_t replyClientId = 0, const char* requestId = nullptr);
bool enqueueLocalScheduleMutation(int idx, const ScheduleConfig& cfg, uint32_t replyClientId = 0, const char* requestId = nullptr);
bool enqueueSourceModeMutation(DataSourceMode mode);
bool enqueueManualFetchMutation(int greenhouseId);
bool enqueueClientEpochMutation(uint32_t epochSec);

#endif
