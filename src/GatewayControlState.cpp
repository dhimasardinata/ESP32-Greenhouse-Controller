#include "GatewayControlState.h"
#include "config.h"
#include <ctime>

namespace
{
GatewayControlState g_cachedGatewayControlState = {};
unsigned long g_cachedGatewayControlStateAtMs = 0;
bool g_cachedGatewayControlStateValid = false;

bool isFogReadyForLocal(const SensorDataManager& sensorData)
{
#if GH_ID_CONFIG == 2
    return sensorData.hasUsableLocalFogData();
#else
    (void)sensorData;
    return true;
#endif
}

bool isFogReadyForCloud(const SensorDataManager& sensorData, unsigned long nowMs)
{
#if GH_ID_CONFIG == 2
    return sensorData.hasFreshCloudFogData(nowMs);
#else
    (void)sensorData;
    (void)nowMs;
    return true;
#endif
}

unsigned long computeSourceAgeMs(unsigned long nowMs,
                                 unsigned long lastUpdateMs)
{
    if (lastUpdateMs == 0)
        return nowMs;
    return nowMs - lastUpdateMs;
}

bool hasRuntimeExplicitSchedule(const RelayController& relay,
                                bool useLocalSchedules,
                                bool cloudScheduleFresh,
                                bool timeValid)
{
    if (!timeValid)
        return false;
    if (!useLocalSchedules && !cloudScheduleFresh)
        return false;

    time_t raw = time(nullptr);
    struct tm* ti = localtime(&raw);
    if (!ti)
        return false;

    return relay.hasAnyExplicitActiveSchedule(ti->tm_hour, ti->tm_min, useLocalSchedules);
}
}

GatewayControlState resolveGatewayControlState(const SensorDataManager& sensorData,
                                               const RelayController& relay,
                                               RTCManager& rtc,
                                               bool netConnected,
                                               unsigned long nowMs)
{
    GatewayControlState state = {};
    state.configuredMode = sensorData.currentMode;
    state.runtimeUsesLocalData = sensorData.isUsingLocalData();
    state.thresholdEditable = sensorData.canEditLocalThresholds();
    state.scheduleEditable = sensorData.canEditLocalSchedules();
    state.edgeScheduleAvailable = relay.hasLocalScheduleProfile() || state.scheduleEditable;
    state.cloudScheduleAvailable = relay.hasCloudScheduleProfile();

    state.localDataUsable = sensorData.getUsableLocalNodeCount() > 0;
    state.localFogReady = isFogReadyForLocal(sensorData);
    state.cloudDataFresh = sensorData.hasFreshCloudSensorData(nowMs);
    state.cloudThresholdFresh = sensorData.hasFreshCloudThresholdProfile(nowMs);
    state.cloudScheduleFresh = relay.hasFreshCloudScheduleProfile(nowMs);
    state.cloudFogReady = isFogReadyForCloud(sensorData, nowMs);

    state.localScheduleConfigured = relay.hasAnyActiveSchedule(true);
    state.cloudScheduleConfigured = relay.hasAnyActiveSchedule(false);
    state.localScheduleRuntimeReady = rtc.isTimeSet() || !state.localScheduleConfigured;
    state.cloudScheduleRuntimeReady = rtc.isTimeSet() || !state.cloudScheduleConfigured;
    state.runtimeScheduleConfigured = state.runtimeUsesLocalData ? state.localScheduleConfigured
                                                                 : state.cloudScheduleConfigured;
    state.runtimeScheduleReady = state.runtimeUsesLocalData ? state.localScheduleRuntimeReady
                                                            : state.cloudScheduleRuntimeReady;
    state.runtimeExplicitScheduleActive = hasRuntimeExplicitSchedule(relay,
                                                                     state.runtimeUsesLocalData,
                                                                     state.cloudScheduleFresh,
                                                                     rtc.isTimeSet());
    state.scheduleOnlyFallbackHealthy = state.runtimeExplicitScheduleActive;

    state.localControlHealthy = state.localDataUsable &&
                                state.localScheduleRuntimeReady &&
                                state.localFogReady;
    state.cloudScheduleControlReady = state.cloudScheduleFresh &&
                                      state.cloudScheduleRuntimeReady;
    state.cloudControlBundleReady = state.cloudThresholdFresh &&
                                    state.cloudScheduleControlReady &&
                                    state.cloudFogReady;
    state.cloudControlHealthy = state.cloudDataFresh &&
                                state.cloudControlBundleReady;

    state.runtimeDataSource = state.runtimeUsesLocalData ? "LOCAL" : "CLOUD";
    state.thresholdRuntimeSource = state.runtimeUsesLocalData ? "EDGE" : "CLOUD";
    state.thresholdConfiguredSource = (state.configuredMode == SOURCE_CLOUD) ? "CLOUD" : "EDGE";
    state.scheduleRuntimeSource = state.runtimeUsesLocalData ? "EDGE" : "CLOUD";
    state.scheduleEffectiveSource = state.runtimeScheduleReady ? state.scheduleRuntimeSource : "DISABLED";

    if (!netConnected || state.configuredMode == SOURCE_LOCAL)
        state.cloudSyncMode = "off";
    else if (state.configuredMode == SOURCE_AUTO && state.runtimeUsesLocalData)
        state.cloudSyncMode = "recovery";
    else
        state.cloudSyncMode = "active";

    if (state.configuredMode == SOURCE_LOCAL)
        state.apiModeValue = 1;
    else if (state.configuredMode == SOURCE_CLOUD)
        state.apiModeValue = 0;
    else
        state.apiModeValue = state.runtimeUsesLocalData ? 1 : 0;

    state.activeSourceHealthy = state.runtimeUsesLocalData ? state.localControlHealthy
                                                           : state.cloudControlHealthy;
    state.activeSourceStale = !state.activeSourceHealthy;

    if (state.activeSourceHealthy)
    {
        state.activeSourceAgeMs = 0;
    }
    else if (state.runtimeUsesLocalData)
    {
        const unsigned long localAgeMs = computeSourceAgeMs(nowMs,
                                                            sensorData.getLastLocalSensorUpdateMs());
        if (!state.localDataUsable || localAgeMs > LOCAL_DATA_VALID_MS)
            state.activeSourceAgeMs = localAgeMs;
        else
            state.activeSourceAgeMs = LOCAL_DATA_VALID_MS + 1;
    }
    else
    {
        const unsigned long cloudAgeMs = computeSourceAgeMs(nowMs,
                                                            sensorData.getLastCloudSensorSuccessMs());
        if (!state.cloudDataFresh || cloudAgeMs > CLOUD_CONTROL_DATA_VALID_MS)
            state.activeSourceAgeMs = cloudAgeMs;
        else
            state.activeSourceAgeMs = CLOUD_CONTROL_DATA_VALID_MS + 1;
    }

    return state;
}

GatewayControlState resolveAndCacheGatewayControlState(const SensorDataManager& sensorData,
                                                       const RelayController& relay,
                                                       RTCManager& rtc,
                                                       bool netConnected,
                                                       unsigned long nowMs)
{
    GatewayControlState state = resolveGatewayControlState(sensorData, relay, rtc, netConnected, nowMs);
    cacheGatewayControlState(state, nowMs);
    return state;
}

void cacheGatewayControlState(const GatewayControlState& state, unsigned long nowMs)
{
    g_cachedGatewayControlState = state;
    g_cachedGatewayControlStateAtMs = nowMs;
    g_cachedGatewayControlStateValid = true;
}

bool tryGetCachedGatewayControlState(GatewayControlState& outState, unsigned long maxAgeMs)
{
    if (!g_cachedGatewayControlStateValid)
        return false;
    if (maxAgeMs > 0)
    {
        const unsigned long ageMs = millis() - g_cachedGatewayControlStateAtMs;
        if (ageMs > maxAgeMs)
            return false;
    }
    outState = g_cachedGatewayControlState;
    return true;
}

bool resolveShouldUseLocalRuntime(const SensorDataManager& sensorData,
                                  const GatewayControlState& state)
{
    if (sensorData.currentMode == SOURCE_LOCAL)
    {
        return state.localControlHealthy;
    }

    if (sensorData.currentMode == SOURCE_CLOUD)
    {
        return false;
    }

    if (sensorData.currentMode != SOURCE_AUTO)
    {
        return false;
    }

    if (!state.localControlHealthy)
    {
        return false;
    }

    if (!state.runtimeUsesLocalData)
    {
        if (!state.cloudControlBundleReady ||
            sensorData.getLastCloudSensorSuccessMs() == 0 ||
            !state.cloudDataFresh ||
            sensorData.getCloudFailureCount() >= AUTO_CLOUD_FAILURE_THRESHOLD)
        {
            return true;
        }
        return false;
    }

    if (state.cloudControlBundleReady &&
        state.cloudDataFresh &&
        sensorData.getCloudSuccessCount() >= AUTO_CLOUD_RECOVERY_THRESHOLD)
    {
        return false;
    }

    return true;
}

bool resolveShouldEnterFailSafe(const GatewayControlState& state,
                                unsigned long lastHealthyControlPathMs,
                                unsigned long nowMs)
{
    bool controlPathHealthy = false;

    switch (state.configuredMode)
    {
    case SOURCE_LOCAL:
        controlPathHealthy = state.localControlHealthy;
        break;
    case SOURCE_CLOUD:
        controlPathHealthy = state.cloudControlHealthy;
        break;
    case SOURCE_AUTO:
    default:
        controlPathHealthy = state.localControlHealthy || state.cloudControlHealthy;
        break;
    }

    if (!controlPathHealthy && state.scheduleOnlyFallbackHealthy)
        controlPathHealthy = true;

    if (controlPathHealthy)
        return false;

    return (lastHealthyControlPathMs == 0) ||
           ((nowMs - lastHealthyControlPathMs) >= CONTROL_SOURCE_LOSS_FAILSAFE_MS);
}
