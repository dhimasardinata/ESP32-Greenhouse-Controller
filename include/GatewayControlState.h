#ifndef GATEWAY_CONTROL_STATE_H
#define GATEWAY_CONTROL_STATE_H

#include "SensorDataManager.h"
#include "RelayController.h"
#include "RTCManager.h"

struct GatewayControlState
{
    DataSourceMode configuredMode = SOURCE_AUTO;
    bool runtimeUsesLocalData = false;
    bool thresholdEditable = true;
    bool scheduleEditable = true;
    bool edgeScheduleAvailable = false;
    bool cloudScheduleAvailable = false;

    bool localDataUsable = false;
    bool localFogReady = false;
    bool cloudDataFresh = false;
    bool cloudThresholdFresh = false;
    bool cloudScheduleFresh = false;
    bool cloudFogReady = true;

    bool localScheduleConfigured = false;
    bool cloudScheduleConfigured = false;
    bool localScheduleRuntimeReady = false;
    bool cloudScheduleRuntimeReady = false;
    bool runtimeScheduleConfigured = false;
    bool runtimeScheduleReady = false;
    bool runtimeExplicitScheduleActive = false;
    bool scheduleOnlyFallbackHealthy = false;

    bool localControlHealthy = false;
    bool cloudScheduleControlReady = false;
    bool cloudControlBundleReady = false;
    bool cloudControlHealthy = false;

    bool activeSourceHealthy = false;
    bool activeSourceStale = true;
    unsigned long activeSourceAgeMs = 0;

    const char* runtimeDataSource = "CLOUD";
    const char* thresholdRuntimeSource = "CLOUD";
    const char* thresholdConfiguredSource = "EDGE";
    const char* scheduleRuntimeSource = "CLOUD";
    const char* scheduleEffectiveSource = "DISABLED";
    const char* cloudSyncMode = "off";
    int apiModeValue = 0;
};

GatewayControlState resolveGatewayControlState(const SensorDataManager& sensorData,
                                               const RelayController& relay,
                                               RTCManager& rtc,
                                               bool netConnected,
                                               unsigned long nowMs);

GatewayControlState resolveAndCacheGatewayControlState(const SensorDataManager& sensorData,
                                                       const RelayController& relay,
                                                       RTCManager& rtc,
                                                       bool netConnected,
                                                       unsigned long nowMs);

void cacheGatewayControlState(const GatewayControlState& state, unsigned long nowMs);
bool tryGetCachedGatewayControlState(GatewayControlState& outState, unsigned long maxAgeMs = 0);

bool resolveShouldUseLocalRuntime(const SensorDataManager& sensorData,
                                  const GatewayControlState& state);

bool resolveShouldEnterFailSafe(const GatewayControlState& state,
                                unsigned long lastHealthyControlPathMs,
                                unsigned long nowMs);

#endif
