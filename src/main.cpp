#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_log.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include "config.h"
#include "LCDDisplay.h"
#include "SensorDataManager.h"
#include "SDCardLogger.h"
#include "RelayController.h"
#include "MyNetworkManager.h"
#include "RTCManager.h"
#include "ConfigManager.h"
#include "WebSocketManager.h"
#include "CryptoUtils.h"
#include "DeferredControlActions.h"
#include "GatewayControlState.h"
#include "SensorNormalization.h"
#include "ScheduleValidation.h"
#include "ThresholdValidation.h"
#include <freertos/FreeRTOS.h>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <math.h>
#define DEBUG_PRINTLN(level, ...)        \
    do                                   \
    {                                    \
        if (DEBUG_LEVEL >= level)        \
        {                                \
            Serial.println(__VA_ARGS__); \
        }                                \
    } while (0)

// --- Global Objects ---
LCDDisplay lcd;
SensorDataManager sensorData;
bool sdCardOk = false;
SDCardLogger sd_logger(lcd, sdCardOk);
RelayController relay(lcd);
MyNetworkManager net(sensorData, lcd, relay);
RTCManager rtc_mgr(lcd, net);
AsyncWebServer server(80);
WebSocketManager wsManager(sensorData, relay, net, rtc_mgr);

namespace
{
    bool g_mdnsStarted = false;
    unsigned long g_lastMdnsRetryMs = 0;
    String g_mdnsHost;
    constexpr unsigned long kInitialNetworkConnectWindowMs = 55000UL;
    constexpr char kDeviceConfigNamespace[] = "device-config";
    constexpr size_t kGatewayUserAgentLen = 40;
    constexpr size_t kGatewayDeviceIdLen = 16;
    constexpr size_t kWebSerialAuthSlotCount = 8;
    constexpr unsigned long kWebSerialAdminSessionTtlMs = 30UL * 60UL * 1000UL;
    constexpr size_t kMutationRequestIdLen = 40;

    struct WebSerialAuthSlot
    {
        uint32_t clientId = 0;
        bool admin = false;
        unsigned long grantedAtMs = 0;
    };

    WebSerialAuthSlot g_webSerialAuthSlots[kWebSerialAuthSlotCount];

    struct ApiDataUploadContext
    {
        char *buffer = nullptr;
        size_t total = 0;
        size_t received = 0;
    };

    enum class PendingControlActionType : uint8_t
    {
        NodeUpdate = 0,
        LocalThresholds,
        LocalSchedule,
        SourceMode,
        ManualFetch,
        ClientEpoch
    };

    struct PendingControlAction
    {
        PendingControlActionType type = PendingControlActionType::NodeUpdate;
        char nodeName[10] = {0};
        float temp = 0.0f;
        float hum = 0.0f;
        float light = 0.0f;
        bool foggy = false;
        float confidence = 0.0f;
        unsigned long tx = 0;
        unsigned long rxEpoch = 0;
        size_t payloadSize = 0;
        int rssiActive = 0;
        int rssiNonActive = 0;
        int scheduleIdx = -1;
        ScheduleConfig scheduleCfg = {0, false, 0, 0, 0, 0, '2', '2', '2'};
        float tMin = 0.0f;
        float tMax = 0.0f;
        float hMin = 0.0f;
        float hMax = 0.0f;
        DataSourceMode sourceMode = SOURCE_AUTO;
        int greenhouseId = 0;
        bool isSensorPayload = false;
        uint32_t epochSec = 0;
        uint32_t replyClientId = 0;
        char requestId[kMutationRequestIdLen] = {0};
    };

    struct PendingQosLog
    {
        char nodeName[10] = {0};
        unsigned long tx = 0;
        size_t payloadSize = 0;
        int rssiActive = 0;
        int rssiNonActive = 0;
    };

    bool waitForInitialNetworkConnection(unsigned long timeoutMs)
    {
        const unsigned long startedAt = millis();
        while ((millis() - startedAt) < timeoutMs)
        {
            esp_task_wdt_reset();
            net.handleWiFi();
            if (net.isConnected())
                return true;
            delay(100);
        }
        return net.isConnected();
    }

    struct PendingNodeUpdate
    {
        char nodeName[10] = {0};
        float temp = 0.0f;
        float hum = 0.0f;
        float light = 0.0f;
        bool foggy = false;
        float confidence = 0.0f;
        unsigned long tx = 0;
        unsigned long rxEpoch = 0;
        size_t payloadSize = 0;
        bool isSensorPayload = false;
    };

    constexpr size_t kPendingControlActionQueueSize = 64;
    PendingControlAction g_pendingControlActions[kPendingControlActionQueueSize];
    size_t g_pendingControlActionHead = 0;
    size_t g_pendingControlActionTail = 0;
    size_t g_pendingControlActionCount = 0;
    portMUX_TYPE g_pendingControlActionMux = portMUX_INITIALIZER_UNLOCKED;
    constexpr size_t kPendingQosLogQueueSize = 64;
    PendingQosLog g_pendingQosLogs[kPendingQosLogQueueSize];
    size_t g_pendingQosLogHead = 0;
    size_t g_pendingQosLogTail = 0;
    size_t g_pendingQosLogCount = 0;
    portMUX_TYPE g_pendingQosLogMux = portMUX_INITIALIZER_UNLOCKED;
    PendingNodeUpdate g_pendingNodeUpdates[MAX_NODES];
    uint16_t g_pendingNodeMask = 0;
    portMUX_TYPE g_pendingNodeMux = portMUX_INITIALIZER_UNLOCKED;
    constexpr uint8_t kRelayStatusSyncCount = 3;
    const char *const kRelayStatusSyncKeys[kRelayStatusSyncCount] = {
        "exhaust_status",
        "dehumidifier_status",
        "blower_status"};
    bool g_pendingRelayStatusValues[kRelayStatusSyncCount] = {false, false, false};
    uint8_t g_pendingRelayStatusMask = 0;
    unsigned long g_lastRelayStatusSyncAttemptMs = 0;

    void buildGatewayUserAgent(char *out, size_t out_len)
    {
        if (!out || out_len == 0)
            return;

        const int written = snprintf(out,
                                     out_len,
                                     "Greenhouse-Atomic-IoT-Gateway-GH%u",
                                     static_cast<unsigned>(GH_ID_CONFIG));
        if (written < 0)
        {
            out[0] = '\0';
            return;
        }
        out[out_len - 1] = '\0';
    }

    void buildGatewayDeviceId(char *out, size_t out_len)
    {
        if (!out || out_len == 0)
            return;

        const int written = snprintf(out,
                                     out_len,
                                     "gateway-gh%u",
                                     static_cast<unsigned>(GH_ID_CONFIG));
        if (written < 0)
        {
            out[0] = '\0';
            return;
        }
        out[out_len - 1] = '\0';
    }

    MyNetworkManager::UplinkMode sanitizeGatewayUplinkMode(uint8_t raw)
    {
        switch (raw)
        {
        case static_cast<uint8_t>(MyNetworkManager::UplinkMode::DIRECT):
            return MyNetworkManager::UplinkMode::DIRECT;
        case static_cast<uint8_t>(MyNetworkManager::UplinkMode::RELAY):
            return MyNetworkManager::UplinkMode::RELAY;
        case static_cast<uint8_t>(MyNetworkManager::UplinkMode::AUTO):
        default:
            return MyNetworkManager::UplinkMode::AUTO;
        }
    }

    void saveGatewayUplinkMode(MyNetworkManager::UplinkMode mode)
    {
        Preferences prefs;
        prefs.begin(kDeviceConfigNamespace, false);
        prefs.putUChar("uplink_mode", static_cast<uint8_t>(mode));
        prefs.end();
    }

    MyNetworkManager::UplinkMode loadGatewayUplinkMode()
    {
        Preferences prefs;
        prefs.begin(kDeviceConfigNamespace, true);
        const uint8_t raw = prefs.getUChar("uplink_mode", static_cast<uint8_t>(MyNetworkManager::UplinkMode::AUTO));
        prefs.end();
        return sanitizeGatewayUplinkMode(raw);
    }

    void cleanupApiDataUploadContext(AsyncWebServerRequest *request)
    {
        if (!request)
            return;
        auto *ctx = static_cast<ApiDataUploadContext *>(request->_tempObject);
        if (!ctx)
            return;
        if (ctx->buffer)
        {
            delete[] ctx->buffer;
            ctx->buffer = nullptr;
        }
        delete ctx;
        request->_tempObject = nullptr;
    }

    WebSerialAuthSlot* findWebSerialAuthSlot(uint32_t clientId)
    {
        const unsigned long now = millis();
        WebSerialAuthSlot* firstEmpty = nullptr;
        WebSerialAuthSlot* firstExpired = nullptr;

        for (size_t i = 0; i < kWebSerialAuthSlotCount; ++i)
        {
            auto& slot = g_webSerialAuthSlots[i];
            if (slot.clientId == clientId)
                return &slot;
            if (slot.clientId == 0 && !firstEmpty)
                firstEmpty = &slot;
            else if (slot.clientId != 0 &&
                     (!slot.admin || (now - slot.grantedAtMs) > kWebSerialAdminSessionTtlMs) &&
                     !firstExpired)
                firstExpired = &slot;
        }

        if (firstEmpty)
            return firstEmpty;
        if (firstExpired)
            return firstExpired;
        return &g_webSerialAuthSlots[0];
    }

    bool isWebSerialAdminClient(AsyncWebSocketClient* client)
    {
        if (!client)
            return false;

        WebSerialAuthSlot* slot = findWebSerialAuthSlot(client->id());
        if (!slot || slot->clientId != client->id())
            return false;

        if (!slot->admin)
            return false;

        if ((millis() - slot->grantedAtMs) > kWebSerialAdminSessionTtlMs)
        {
            slot->clientId = 0;
            slot->admin = false;
            slot->grantedAtMs = 0;
            return false;
        }

        return true;
    }

    void setWebSerialAdminClient(AsyncWebSocketClient* client, bool admin)
    {
        if (!client)
            return;

        WebSerialAuthSlot* slot = findWebSerialAuthSlot(client->id());
        if (!slot)
            return;

        slot->clientId = client->id();
        slot->admin = admin;
        slot->grantedAtMs = admin ? millis() : 0;
    }

    void clearAllWebSerialAdminSessions()
    {
        for (auto& slot : g_webSerialAuthSlots)
        {
            slot.clientId = 0;
            slot.admin = false;
            slot.grantedAtMs = 0;
        }
    }

    String getWebSocketClientBinding(AsyncWebSocketClient* client)
    {
        if (!client)
            return "";
        return client->remoteIP().toString();
    }

    unsigned long parseDateTimeStringToEpoch(const char *dateTime)
    {
        if (!dateTime || dateTime[0] == '\0')
            return 0UL;

        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (sscanf(dateTime, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
            return 0UL;

        struct tm t = {};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;
        t.tm_isdst = -1;

        const time_t epoch = mktime(&t);
        return (epoch > 0) ? static_cast<unsigned long>(epoch) : 0UL;
    }

    unsigned long extractTxEpoch(const JsonDocument &doc)
    {
        unsigned long tx = 0UL;

        if (doc["send_time"].is<unsigned long>())
        {
            tx = doc["send_time"].as<unsigned long>();
        }
        else if (doc["send_time"].is<long>())
        {
            const long v = doc["send_time"].as<long>();
            if (v > 0)
                tx = static_cast<unsigned long>(v);
        }
        else if (doc["send_time"].is<const char *>())
        {
            tx = parseDateTimeStringToEpoch(doc["send_time"].as<const char *>());
        }

        if (tx == 0UL)
        {
            tx = doc["timestamp"] | 0UL;
        }

        return tx;
    }

    bool parseStrictJsonFloat(JsonVariantConst value, float &outValue)
    {
        if (value.isNull())
            return false;

        if (value.is<float>() || value.is<double>() ||
            value.is<int>() || value.is<long>() ||
            value.is<unsigned int>() || value.is<unsigned long>())
        {
            outValue = value.as<float>();
            return isfinite(outValue);
        }

        if (value.is<const char *>())
        {
            const char *raw = value.as<const char *>();
            if (!raw || raw[0] == '\0')
                return false;

            char *endPtr = nullptr;
            const float parsed = strtof(raw, &endPtr);
            if (endPtr == raw || !isfinite(parsed))
                return false;

            while (*endPtr != '\0' && isspace(static_cast<unsigned char>(*endPtr)))
                ++endPtr;
            if (*endPtr != '\0')
                return false;

            outValue = parsed;
            return true;
        }

        return false;
    }

    bool parseLocalSensorPayload(const JsonDocument &doc, float &temp, float &hum, float &light)
    {
        if (!parseStrictJsonFloat(doc["temperature"], temp) || !SensorNormalization::normalizeTemperature(temp))
            return false;
        if (!parseStrictJsonFloat(doc["humidity"], hum) || !SensorNormalization::normalizeHumidity(hum))
            return false;

        const JsonVariantConst lightIntensity = doc["light_intensity"];
        const JsonVariantConst luxValue = doc["lux"];
        const bool hasLightField = !lightIntensity.isNull() || !luxValue.isNull();
        if (!hasLightField)
        {
            // Node normal mengirim field light. Jika temp+hum sama-sama 0 tanpa light,
            // anggap ini payload fallback/invalid, bukan data lingkungan yang valid.
            if (temp == 0.0f && hum == 0.0f)
                return false;
            light = NAN;
            return true;
        }

        float parsedLight = NAN;
        const bool lightParsed = parseStrictJsonFloat(lightIntensity, parsedLight) ||
                                 parseStrictJsonFloat(luxValue, parsedLight);
        if (!lightParsed || !SensorNormalization::normalizeLight(parsedLight))
            return false;

        // Di firmware node, data sensor invalid dibungkus menjadi 0/0/0.
        // Tetap izinkan lux=0 saat malam, asalkan temp/hum bukan sentinel invalid.
        if (temp == 0.0f && hum == 0.0f && parsedLight == 0.0f)
            return false;

        light = parsedLight;
        return true;
    }

    bool ensureMdnsService()
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            if (g_mdnsStarted)
            {
                MDNS.end();
                g_mdnsStarted = false;
                Serial.println("[MDNS] Stopped (WiFi disconnected)");
            }
            return false;
        }

        if (g_mdnsHost.isEmpty())
        {
            g_mdnsHost = "gateway-gh-" + String(GH_ID_CONFIG);
        }

        const unsigned long nowMs = millis();
        if (g_mdnsStarted)
        {
            return true;
        }
        if (g_lastMdnsRetryMs != 0UL && (nowMs - g_lastMdnsRetryMs) < 3000UL)
        {
            return false;
        }
        g_lastMdnsRetryMs = nowMs;

        if (!MDNS.begin(g_mdnsHost.c_str()))
        {
            Serial.printf("[MDNS] Begin failed: %s.local\n", g_mdnsHost.c_str());
            return false;
        }

        MDNS.addService("http", "tcp", 80);
        g_mdnsStarted = true;
        Serial.printf("[MDNS] Started: http://%s.local/\n", g_mdnsHost.c_str());
        return true;
    }
}

// --- State Variables ---
unsigned long lastLoop = 0;
unsigned long lastApiAttempt = 0;
unsigned long lastTimeSync = 0;
unsigned long lastLocalNodeUpdate = 0;
unsigned long lastNetworkAvailable = 0;
unsigned long lastSdRetry = 0;
bool isInFailSafeMode = false;
unsigned long lastConnectionRetry = 0;
unsigned long currentRetryDelay = INITIAL_RETRY_DELAY_MS;
unsigned long lastWiFiRetryWhenGprs = 0;
unsigned long lastUpdateCheck = 0;
unsigned long lastWsBroadcast = 0;
unsigned long lastScheduleFetch = 0;
char active_schedule_url[128];
int apiStep = 0;
unsigned long lastStepMillis = 0;
unsigned long lastHealthyControlPathMs = 0;
bool apiRecoveryNodeOk = false;
bool apiRecoveryThresholdOk = false;
bool apiRecoveryScheduleOk = false;
bool pendingCloudPrimeFetch = false;
int pendingManualFetchGhId = 0;
bool pendingRtcDriftCheck = false;
bool pendingOtaCheck = false;
unsigned long lastRtcSyncAttempt = 0;

// Forward Declarations
void checkForUpdates(uint16_t timeoutMs, bool allowApply);
void runControlLogic();
void updateEffectiveDataSource(unsigned long now);
GatewayControlState getControlState(unsigned long now);
bool processDeferredNetworkActions();

namespace
{
void processPendingControlActions();
bool isCloudControlSensorSnapshotReady(const CloudSensorSnapshot& snapshot);
bool isCloudControlThresholdSnapshotReady(const CloudThresholdSnapshot& snapshot);

#if GH_ID_CONFIG == 2
constexpr uint8_t kManualFetchRequestCount = 3;
#else
constexpr uint8_t kManualFetchRequestCount = 2;
#endif
constexpr unsigned long kManualFetchExtraOverheadMs = 500UL;

uint16_t computeControlSafeHttpTimeout(unsigned long nowMs,
                                       uint16_t preferredTimeoutMs = HTTP_CONTROL_SAFE_TIMEOUT_MS,
                                       uint8_t sequentialRequestCount = 1,
                                       unsigned long extraOverheadMs = 0UL)
{
    if (sequentialRequestCount == 0)
        sequentialRequestCount = 1;

    const unsigned long nextControlDueMs = lastLoop + LOOP_MS;
    const unsigned long msUntilNextControl = (nowMs >= nextControlDueMs) ? 0UL : (nextControlDueMs - nowMs);
    const unsigned long requiredReserveMs = CONTROL_LOOP_SAFETY_MARGIN_MS + extraOverheadMs;
    if (msUntilNextControl <= requiredReserveMs)
        return 0;

    unsigned long perRequestBudgetMs = (msUntilNextControl - requiredReserveMs) / sequentialRequestCount;
    if (perRequestBudgetMs > preferredTimeoutMs)
        perRequestBudgetMs = preferredTimeoutMs;
    if (perRequestBudgetMs > HTTP_CONTROL_SAFE_TIMEOUT_MS)
        perRequestBudgetMs = HTTP_CONTROL_SAFE_TIMEOUT_MS;
    if (perRequestBudgetMs < HTTP_CONTROL_MIN_TIMEOUT_MS)
        return 0;

    return static_cast<uint16_t>(perRequestBudgetMs);
}

uint16_t computeControlSafeBlockingBudget(unsigned long nowMs)
{
    const unsigned long nextControlDueMs = lastLoop + LOOP_MS;
    if (nowMs >= nextControlDueMs)
        return 0;

    const unsigned long msUntilNextControl = nextControlDueMs - nowMs;
    if (msUntilNextControl <= CONTROL_LOOP_SAFETY_MARGIN_MS)
        return 0;

    const unsigned long availableBudgetMs = msUntilNextControl - CONTROL_LOOP_SAFETY_MARGIN_MS;
    if (availableBudgetMs > 0xFFFFUL)
        return 0xFFFFU;
    return static_cast<uint16_t>(availableBudgetMs);
}

void applyCloudSensorSnapshot(const CloudSensorSnapshot& snapshot)
{
    sensorData.updateFromCloudPartial(snapshot.hasTemp,
                                      snapshot.temp,
                                      snapshot.hasHum,
                                      snapshot.hum,
                                      snapshot.hasLight,
                                      snapshot.light);
}

void applyCloudThresholdSnapshot(const CloudThresholdSnapshot& snapshot)
{
    if (!isCloudControlThresholdSnapshotReady(snapshot))
        return;

    sensorData.updateThresholds(snapshot.tempMin,
                                snapshot.tempMax,
                                snapshot.humMin,
                                snapshot.humMax,
                                snapshot.lightMin,
                                snapshot.lightMax);
}

void applyCloudScheduleSnapshot(const CloudScheduleSnapshot& snapshot)
{
    if (!snapshot.valid)
        return;

    relay.applyCloudSchedules(snapshot.schedules, MAX_SCHEDULES);
}

void applyCloudFogSnapshot(const CloudFogSnapshot& snapshot)
{
#if GH_ID_CONFIG == 2
    if (snapshot.valid)
        sensorData.setCloudFog(snapshot.foggy);
#else
    (void)snapshot;
#endif
}

bool isCloudControlSensorSnapshotReady(const CloudSensorSnapshot& snapshot)
{
    // Kontrol relay hanya aman jika temperatur dan kelembapan cloud sama-sama fresh.
    return snapshot.controlReady;
}

String describeCloudControlSensorGap(const CloudSensorSnapshot& snapshot)
{
    String fields = "";
    if (!snapshot.hasTemp)
        fields += "temperature";
    if (!snapshot.hasHum)
    {
        if (fields.length() > 0)
            fields += ", ";
        fields += "humidity";
    }
    if (fields.length() == 0)
        fields = "none";
    return fields;
}

bool isCloudControlThresholdSnapshotReady(const CloudThresholdSnapshot& snapshot)
{
    return snapshot.valid && snapshot.hasTemp && snapshot.hasHum;
}

bool serviceDeferredMaintenance(unsigned long nowMs)
{
    if (pendingRtcDriftCheck)
    {
        if (!net.isConnected())
            return false;
        if (lastRtcSyncAttempt != 0 && (nowMs - lastRtcSyncAttempt) < RTC_SYNC_RETRY_MS)
            return false;

        const uint16_t timeoutMs = computeControlSafeHttpTimeout(nowMs);
        if (timeoutMs == 0)
            return false;

        lastRtcSyncAttempt = nowMs;
        const RtcSyncResult syncResult = rtc_mgr.checkAndSyncOnDrift(timeoutMs);
        if (syncResult == RtcSyncResult::Ntp ||
            syncResult == RtcSyncResult::ModemNetwork ||
            syncResult == RtcSyncResult::Http)
        {
            pendingRtcDriftCheck = false;
            lastTimeSync = nowMs;
            wsManager.broadcastStatus();
        }
        else if (syncResult == RtcSyncResult::ClientBootstrap)
        {
            lastTimeSync = nowMs;
            wsManager.broadcastStatus();
        }
        return true;
    }

    if (pendingOtaCheck)
    {
        if (sd_logger.isBusy() || !net.isWiFiConnected())
            return false;

        const uint16_t timeoutMs = computeControlSafeHttpTimeout(nowMs);
        if (timeoutMs == 0)
            return false;

        pendingOtaCheck = false;
        checkForUpdates(timeoutMs, AUTO_OTA_APPLY_ENABLED);
        return true;
    }

    return false;
}

bool enqueuePendingControlAction(const PendingControlAction& action)
{
    bool queued = false;
    portENTER_CRITICAL(&g_pendingControlActionMux);
    if (g_pendingControlActionCount < kPendingControlActionQueueSize)
    {
        g_pendingControlActions[g_pendingControlActionTail] = action;
        g_pendingControlActionTail = (g_pendingControlActionTail + 1) % kPendingControlActionQueueSize;
        g_pendingControlActionCount++;
        queued = true;
    }
    portEXIT_CRITICAL(&g_pendingControlActionMux);
    return queued;
}

bool dequeuePendingControlAction(PendingControlAction& action)
{
    bool dequeued = false;
    portENTER_CRITICAL(&g_pendingControlActionMux);
    if (g_pendingControlActionCount > 0)
    {
        action = g_pendingControlActions[g_pendingControlActionHead];
        g_pendingControlActionHead = (g_pendingControlActionHead + 1) % kPendingControlActionQueueSize;
        g_pendingControlActionCount--;
        dequeued = true;
    }
    portEXIT_CRITICAL(&g_pendingControlActionMux);
    return dequeued;
}

bool enqueuePendingQosLog(const PendingQosLog& entry)
{
    bool queued = false;
    portENTER_CRITICAL(&g_pendingQosLogMux);
    if (g_pendingQosLogCount < kPendingQosLogQueueSize)
    {
        g_pendingQosLogs[g_pendingQosLogTail] = entry;
        g_pendingQosLogTail = (g_pendingQosLogTail + 1) % kPendingQosLogQueueSize;
        g_pendingQosLogCount++;
        queued = true;
    }
    portEXIT_CRITICAL(&g_pendingQosLogMux);
    return queued;
}

bool dequeuePendingQosLog(PendingQosLog& entry)
{
    bool dequeued = false;
    portENTER_CRITICAL(&g_pendingQosLogMux);
    if (g_pendingQosLogCount > 0)
    {
        entry = g_pendingQosLogs[g_pendingQosLogHead];
        g_pendingQosLogHead = (g_pendingQosLogHead + 1) % kPendingQosLogQueueSize;
        g_pendingQosLogCount--;
        dequeued = true;
    }
    portEXIT_CRITICAL(&g_pendingQosLogMux);
    return dequeued;
}

int relayStatusSyncSlotFromRelayIndex(RelayIndex relayIndex)
{
    switch (relayIndex)
    {
    case RELAY_EXHAUST:
        return 0;
    case RELAY_DEHUMIDIFIER:
        return 1;
    case RELAY_BLOWER:
        return 2;
    default:
        return -1;
    }
}

void queueRelayStatusSync(RelayIndex relayIndex, bool state)
{
    const int slot = relayStatusSyncSlotFromRelayIndex(relayIndex);
    if (slot < 0)
        return;

    g_pendingRelayStatusValues[slot] = state;
    g_pendingRelayStatusMask |= static_cast<uint8_t>(1U << slot);
}

void queueRelayStatusResyncAll()
{
    queueRelayStatusSync(RELAY_EXHAUST, relay.getR1());
    queueRelayStatusSync(RELAY_DEHUMIDIFIER, relay.getR2());
    queueRelayStatusSync(RELAY_BLOWER, relay.getR3());
}

bool processPendingRelayStatusSync()
{
    if (g_pendingRelayStatusMask == 0 || !net.isConnected())
        return false;

    const unsigned long nowMs = millis();
    const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(nowMs);
    if (requestTimeoutMs == 0)
        return false;
    if (g_lastRelayStatusSyncAttemptMs != 0 &&
        (nowMs - g_lastRelayStatusSyncAttemptMs) < RELAY_STATUS_SYNC_RETRY_MS)
        return false;

    for (uint8_t slot = 0; slot < kRelayStatusSyncCount; ++slot)
    {
        const uint8_t bit = static_cast<uint8_t>(1U << slot);
        if ((g_pendingRelayStatusMask & bit) == 0)
            continue;

        g_lastRelayStatusSyncAttemptMs = nowMs;
        if (net.postSingleDeviceStatus(GH_ID_CONFIG, kRelayStatusSyncKeys[slot], g_pendingRelayStatusValues[slot], requestTimeoutMs))
            g_pendingRelayStatusMask &= static_cast<uint8_t>(~bit);
        return true;
    }

    return false;
}

void runControlLoopIfDue(unsigned long nowMs)
{
    if (nowMs - lastLoop >= LOOP_MS)
    {
        lastLoop = nowMs;
        runControlLogic();
    }
}

void serviceCriticalControlPath()
{
    processPendingControlActions();
    runControlLoopIfDue(millis());
}

int nodeNameToPendingSlot(const char* nodeName)
{
    if (!nodeName || nodeName[0] == '\0')
        return -1;

    if (strcmp(nodeName, "cam-1") == 0)
        return 10;
    if (strcmp(nodeName, "cam-2") == 0)
        return 11;

    if (strncmp(nodeName, "node-", 5) != 0)
        return -1;

    const int nodeIndex = atoi(nodeName + 5) - 1;
    if (nodeIndex < 0 || nodeIndex >= 10)
        return -1;
    return nodeIndex;
}

bool enqueueLatestNodeUpdate(const PendingNodeUpdate& update)
{
    const int slot = nodeNameToPendingSlot(update.nodeName);
    if (slot < 0 || slot >= MAX_NODES)
        return false;

    portENTER_CRITICAL(&g_pendingNodeMux);
    g_pendingNodeUpdates[slot] = update;
    g_pendingNodeMask |= static_cast<uint16_t>(1U << slot);
    portEXIT_CRITICAL(&g_pendingNodeMux);
    return true;
}

bool processPendingNodeUpdates()
{
    PendingNodeUpdate snapshot[MAX_NODES];
    uint16_t mask = 0;

    portENTER_CRITICAL(&g_pendingNodeMux);
    mask = g_pendingNodeMask;
    if (mask != 0)
    {
        for (int i = 0; i < MAX_NODES; ++i)
        {
            if ((mask & static_cast<uint16_t>(1U << i)) == 0)
                continue;
            snapshot[i] = g_pendingNodeUpdates[i];
        }
        g_pendingNodeMask = 0;
    }
    portEXIT_CRITICAL(&g_pendingNodeMux);

    if (mask == 0)
        return false;

    for (int i = 0; i < MAX_NODES; ++i)
    {
        if ((mask & static_cast<uint16_t>(1U << i)) == 0)
            continue;

        const PendingNodeUpdate& update = snapshot[i];
        sensorData.updateFromNode(update.nodeName,
                                  update.temp,
                                  update.hum,
                                  update.light,
                                  update.foggy,
                                  update.confidence,
                                  update.tx,
                                  update.rxEpoch,
                                  update.payloadSize);
        const unsigned long localRxMs = millis();
        lastLocalNodeUpdate = localRxMs;
        if (update.isSensorPayload)
            sensorData.noteLocalSensorUpdate(localRxMs);
    }

    return true;
}

bool enqueueNodeMutation(const char* nodeName, float temp, float hum, float light, bool foggy, float confidence,
                         unsigned long tx, unsigned long rxEpoch, size_t payloadSize, bool isSensorPayload)
{
    if (!nodeName || nodeName[0] == '\0')
        return false;

    PendingNodeUpdate update = {};
    strncpy(update.nodeName, nodeName, sizeof(update.nodeName) - 1);
    update.nodeName[sizeof(update.nodeName) - 1] = '\0';
    update.temp = temp;
    update.hum = hum;
    update.light = light;
    update.foggy = foggy;
    update.confidence = confidence;
    update.tx = tx;
    update.rxEpoch = rxEpoch;
    update.payloadSize = payloadSize;
    update.isSensorPayload = isSensorPayload;
    return enqueueLatestNodeUpdate(update);
}

bool enqueueQosLogMutation(const char* nodeName, unsigned long tx, size_t payloadSize, int rssiActive, int rssiNonActive)
{
    if (!nodeName || nodeName[0] == '\0')
        return false;

    PendingQosLog entry = {};
    strncpy(entry.nodeName, nodeName, sizeof(entry.nodeName) - 1);
    entry.nodeName[sizeof(entry.nodeName) - 1] = '\0';
    entry.tx = tx;
    entry.payloadSize = payloadSize;
    entry.rssiActive = rssiActive;
    entry.rssiNonActive = rssiNonActive;
    return enqueuePendingQosLog(entry);
}

void processPendingControlActions()
{
    PendingControlAction action;
    bool shouldBroadcast = processPendingNodeUpdates();
    size_t iterationBudget = 0;

    portENTER_CRITICAL(&g_pendingControlActionMux);
    iterationBudget = g_pendingControlActionCount;
    portEXIT_CRITICAL(&g_pendingControlActionMux);

    while (iterationBudget-- > 0 && dequeuePendingControlAction(action))
    {
        switch (action.type)
        {
        case PendingControlActionType::NodeUpdate:
        {
            PendingNodeUpdate update = {};
            strncpy(update.nodeName, action.nodeName, sizeof(update.nodeName) - 1);
            update.nodeName[sizeof(update.nodeName) - 1] = '\0';
            update.temp = action.temp;
            update.hum = action.hum;
            update.light = action.light;
            update.foggy = action.foggy;
            update.confidence = action.confidence;
            update.tx = action.tx;
            update.rxEpoch = action.rxEpoch;
            update.payloadSize = action.payloadSize;
            update.isSensorPayload = action.isSensorPayload;
            enqueueLatestNodeUpdate(update);
            shouldBroadcast = true;
            break;
        }
        case PendingControlActionType::LocalThresholds:
        {
            bool applied = false;
            const char* message = "Threshold update ditolak gateway.";
            if (!sensorData.canEditLocalThresholds())
            {
                message = "Thresholds read-only pada mode saat ini.";
            }
            else if (!ThresholdValidation::isTemperatureRangeValid(action.tMin, action.tMax))
            {
                message = "Threshold suhu harus di rentang -20..80 dengan min < max.";
            }
            else if (!ThresholdValidation::isHumidityRangeValid(action.hMin, action.hMax))
            {
                message = "Threshold humidity harus di rentang 0..100 dengan min < max.";
            }
            else if (sensorData.setLocalThresholds(action.tMin, action.tMax, action.hMin, action.hMax))
            {
                applied = true;
                message = "Threshold update sudah diterapkan gateway.";
                shouldBroadcast = true;
            }
            if (action.replyClientId != 0)
                wsManager.sendMutationResultToClient(action.replyClientId, "set_thresholds_result", applied, message, -1, action.requestId);
            break;
        }
        case PendingControlActionType::LocalSchedule:
        {
            bool applied = false;
            const char* message = "Schedule update ditolak gateway.";
            if (!sensorData.canEditLocalSchedules())
            {
                message = "Schedule read-only pada mode saat ini.";
            }
            else if (action.scheduleIdx < 0 || action.scheduleIdx >= MAX_SCHEDULES)
            {
                message = "Index schedule tidak valid.";
            }
            else if (!ScheduleValidation::isScheduleConfigValid(action.scheduleCfg))
            {
                message = "Schedule aktif harus punya waktu valid dan durasi non-zero.";
            }
            else if (relay.saveSchedule(action.scheduleIdx, action.scheduleCfg))
            {
                applied = true;
                message = "Schedule update sudah diterapkan gateway.";
                shouldBroadcast = true;
            }
            if (action.replyClientId != 0)
                wsManager.sendMutationResultToClient(action.replyClientId, "set_schedule_result", applied, message, action.scheduleIdx, action.requestId);
            break;
        }
        case PendingControlActionType::SourceMode:
        {
            const unsigned long nowMs = millis();
            apiStep = 0;
            apiRecoveryNodeOk = false;
            apiRecoveryThresholdOk = false;
            apiRecoveryScheduleOk = false;
            sensorData.setSourceMode(action.sourceMode);
            updateEffectiveDataSource(nowMs);
            pendingCloudPrimeFetch = (action.sourceMode != SOURCE_LOCAL);
            if (action.sourceMode != SOURCE_LOCAL)
            {
                lastApiAttempt = nowMs - API_MS;
                lastScheduleFetch = nowMs - API_MS;
            }
            shouldBroadcast = true;
            break;
        }
        case PendingControlActionType::ManualFetch:
            if (action.greenhouseId != GH_ID_CONFIG)
            {
                WebSerial.println("[BLOCKED] Manual fetch only allowed for current GH ID to protect live control state.");
                break;
            }
            pendingManualFetchGhId = action.greenhouseId;
            break;
        case PendingControlActionType::ClientEpoch:
        {
            const unsigned long nowMs = millis();
            rtc_mgr.noteClientEpochSample(action.epochSec, nowMs);
            pendingRtcDriftCheck = true;
            if (!rtc_mgr.isTimeSet() && rtc_mgr.syncFromRecentClientEpoch(nowMs))
            {
                lastTimeSync = nowMs;
                shouldBroadcast = true;
            }
            break;
        }
        }
    }

    if (shouldBroadcast)
        wsManager.broadcastStatus();
}

void processPendingQosLogs()
{
    if (sd_logger.isBusy() || !sdCardOk)
        return;

    PendingQosLog entry;
    size_t iterationBudget = 4;

    while (iterationBudget-- > 0 && dequeuePendingQosLog(entry))
    {
        if (sdCardOk && entry.tx > 0)
            sd_logger.logQoS(entry.nodeName, entry.tx, entry.payloadSize, entry.rssiActive, entry.rssiNonActive);
    }
}
}

bool processDeferredNetworkActions()
{
    serviceCriticalControlPath();

    if (apiStep != 0)
        return false;
    if (sd_logger.isBusy())
        return false;

    if (pendingManualFetchGhId != 0)
    {
        if (!net.isConnected())
            return false;

        const uint16_t manualFetchTimeoutMs = computeControlSafeHttpTimeout(
            millis(),
            HTTP_CONTROL_SAFE_TIMEOUT_MS,
            kManualFetchRequestCount,
            kManualFetchExtraOverheadMs);
        if (manualFetchTimeoutMs == 0)
            return false;

        const int greenhouseId = pendingManualFetchGhId;
        pendingManualFetchGhId = 0;

        WebSerial.printf("Manual fetching data for GH ID: %d...\n", greenhouseId);
        CloudSensorSnapshot sensorSnapshot = {};
        CloudThresholdSnapshot thresholdSnapshot = {};
#if GH_ID_CONFIG == 2
        CloudFogSnapshot fogSnapshot = {};
        CloudFogSnapshot* fogSnapshotPtr = &fogSnapshot;
#else
        CloudFogSnapshot* fogSnapshotPtr = nullptr;
#endif
        const bool bundleFetchOk = net.fetchBundleForGreenhouse(greenhouseId,
                                                                sensorSnapshot,
                                                                thresholdSnapshot,
                                                                fogSnapshotPtr,
                                                                WebSerial,
                                                                manualFetchTimeoutMs);
        if (bundleFetchOk)
        {
            applyCloudSensorSnapshot(sensorSnapshot);
            applyCloudThresholdSnapshot(thresholdSnapshot);
#if GH_ID_CONFIG == 2
            applyCloudFogSnapshot(fogSnapshot);
#endif
        }

        if (bundleFetchOk &&
            isCloudControlSensorSnapshotReady(sensorSnapshot))
        {
            WebSerial.println("[OK] Sensor Data");
            WebSerial.println("[OK] Thresholds");
#if GH_ID_CONFIG == 2
            WebSerial.println("[OK] Camera");
#endif
            const unsigned long cloudSuccessMs = millis();
            sensorData.noteCloudSensorSuccess(cloudSuccessMs);
            updateEffectiveDataSource(cloudSuccessMs);
            WebSerial.println("[SUCCESS] Data applied to local memory.");
            wsManager.broadcastStatus();
        }
        else
        {
            if (bundleFetchOk)
            {
                WebSerial.printf("[FAILED] Cloud control payload incomplete; missing/invalid: %s.\n",
                                 describeCloudControlSensorGap(sensorSnapshot).c_str());
                WebSerial.println("[INFO] Valid cloud fields were cached, but control path stayed degraded.");
                wsManager.broadcastStatus();
            }
            else
                WebSerial.println("[FAILED] Could not fetch data from server.");
        }
        return true;
    }

    if (pendingCloudPrimeFetch)
    {
        if (!net.isConnected())
            return false;

        const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(millis(),
                                                                        HTTP_CONTROL_SAFE_TIMEOUT_MS,
                                                                        2);
        if (requestTimeoutMs == 0)
            return false;

        pendingCloudPrimeFetch = false;
        CloudSensorSnapshot sensorSnapshot = {};
        CloudThresholdSnapshot thresholdSnapshot = {};
        const bool sensorFetchOk = net.fetchNodeData(sensorSnapshot, requestTimeoutMs);
        const bool thresholdFetchOk = net.fetchThresholds(thresholdSnapshot, requestTimeoutMs);
        if (sensorFetchOk)
            applyCloudSensorSnapshot(sensorSnapshot);
        if (thresholdFetchOk)
            applyCloudThresholdSnapshot(thresholdSnapshot);
        if (sensorFetchOk &&
            isCloudControlSensorSnapshotReady(sensorSnapshot))
        {
            sensorData.noteCloudSensorSuccess(millis());
        }
        else if (sensorFetchOk)
        {
            Serial.printf("[ND] Cloud control payload incomplete during prime fetch; missing/invalid: %s\n",
                          describeCloudControlSensorGap(sensorSnapshot).c_str());
        }
        if (!thresholdFetchOk)
        {
            Serial.println("[TH] Prime fetch threshold gagal; gunakan cache/default sementara.");
        }
        updateEffectiveDataSource(millis());
        wsManager.broadcastStatus();
        return true;
    }

    return processPendingRelayStatusSync();
}

bool enqueueLocalThresholdMutation(float tMin, float tMax, float hMin, float hMax, uint32_t replyClientId, const char* requestId)
{
    PendingControlAction action = {};
    action.type = PendingControlActionType::LocalThresholds;
    action.tMin = tMin;
    action.tMax = tMax;
    action.hMin = hMin;
    action.hMax = hMax;
    action.replyClientId = replyClientId;
    if (requestId)
    {
        strncpy(action.requestId, requestId, sizeof(action.requestId) - 1);
        action.requestId[sizeof(action.requestId) - 1] = '\0';
    }
    return enqueuePendingControlAction(action);
}

bool enqueueLocalScheduleMutation(int idx, const ScheduleConfig& cfg, uint32_t replyClientId, const char* requestId)
{
    PendingControlAction action = {};
    action.type = PendingControlActionType::LocalSchedule;
    action.scheduleIdx = idx;
    action.scheduleCfg = cfg;
    action.replyClientId = replyClientId;
    if (requestId)
    {
        strncpy(action.requestId, requestId, sizeof(action.requestId) - 1);
        action.requestId[sizeof(action.requestId) - 1] = '\0';
    }
    return enqueuePendingControlAction(action);
}

bool enqueueSourceModeMutation(DataSourceMode mode)
{
    PendingControlAction action = {};
    action.type = PendingControlActionType::SourceMode;
    action.sourceMode = mode;
    return enqueuePendingControlAction(action);
}

bool enqueueManualFetchMutation(int greenhouseId)
{
    PendingControlAction action = {};
    action.type = PendingControlActionType::ManualFetch;
    action.greenhouseId = greenhouseId;
    return enqueuePendingControlAction(action);
}

bool enqueueClientEpochMutation(uint32_t epochSec)
{
    if (epochSec <= 1672531200UL)
        return false;

    PendingControlAction action = {};
    action.type = PendingControlActionType::ClientEpoch;
    action.epochSec = epochSec;
    return enqueuePendingControlAction(action);
}

int compareVersionStrings(const String& a, const String& b)
{
    int i = 0, j = 0;
    const int aLen = a.length();
    const int bLen = b.length();
    while (i < aLen || j < bLen)
    {
        long av = 0;
        long bv = 0;

        while (i < aLen && a[i] == '.') i++;
        while (i < aLen && isDigit(a[i]))
        {
            av = av * 10 + (a[i] - '0');
            i++;
        }

        while (j < bLen && b[j] == '.') j++;
        while (j < bLen && isDigit(b[j]))
        {
            bv = bv * 10 + (b[j] - '0');
            j++;
        }

        if (av > bv) return 1;
        if (av < bv) return -1;

        while (i < aLen && a[i] != '.' && !isDigit(a[i])) i++;
        while (j < bLen && b[j] != '.' && !isDigit(b[j])) j++;
    }
    return 0;
}

GatewayControlState getControlState(unsigned long now)
{
    GatewayControlState state = {};
    if (tryGetCachedGatewayControlState(state, LOOP_MS))
        return state;
    return resolveAndCacheGatewayControlState(sensorData, relay, rtc_mgr, net.isConnected(), now);
}

bool shouldUseLocalRuntime(unsigned long now)
{
    return resolveShouldUseLocalRuntime(sensorData,
                                        resolveGatewayControlState(sensorData, relay, rtc_mgr, net.isConnected(), now));
}

enum CloudSyncMode : uint8_t
{
    CLOUD_SYNC_OFF = 0,
    CLOUD_SYNC_ACTIVE,
    CLOUD_SYNC_RECOVERY
};

CloudSyncMode apiSyncMode = CLOUD_SYNC_OFF;

CloudSyncMode getCloudSyncMode(unsigned long now)
{
    const GatewayControlState state = getControlState(now);
    if (strcmp(state.cloudSyncMode, "recovery") == 0)
        return CLOUD_SYNC_RECOVERY;
    if (strcmp(state.cloudSyncMode, "active") == 0)
        return CLOUD_SYNC_ACTIVE;

    return CLOUD_SYNC_OFF;
}

const char* cloudSyncModeToString(CloudSyncMode mode)
{
    switch (mode)
    {
    case CLOUD_SYNC_ACTIVE:
        return "CLOUD ACTIVE";
    case CLOUD_SYNC_RECOVERY:
        return "RECOVERY PROBE";
    case CLOUD_SYNC_OFF:
    default:
        return "LOCAL ONLY";
    }
}

bool shouldTriggerCloudFetch(unsigned long now)
{
    const CloudSyncMode syncMode = getCloudSyncMode(now);
    if (syncMode == CLOUD_SYNC_OFF)
        return false;

    const unsigned long intervalMs = (syncMode == CLOUD_SYNC_RECOVERY)
                                         ? AUTO_CLOUD_RETRY_INTERVAL_MS
                                         : API_MS;
    return (now - lastApiAttempt >= intervalMs);
}

void updateEffectiveDataSource(unsigned long now)
{
    sensorData.setRuntimeFallbackToLocal(shouldUseLocalRuntime(now));
    resolveAndCacheGatewayControlState(sensorData, relay, rtc_mgr, net.isConnected(), now);
}

// --- WebSerial Functions ---
void printWebSerialHelp(bool webSerialAdmin)
{
    String h = "";
    h += "\n";
    h += "=================================================\n";
    h += "               AVAILABLE COMMANDS                \n";
    h += "=================================================\n";

    h += "\n[ PUBLIC COMMANDS ]\n";
    h += "  status            : Show system summary\n";
    h += "  sdcard_status     : Check SD storage usage\n";
    h += "  memo_status       : Check RAM (Heap) usage\n";
    h += "  help              : Show this menu\n";

    h += "\n[ NETWORK QoS DIAGNOSTIC ]\n";
    h += "  qos_th            : Test Threshold URL\n";
    h += "  qos_nd            : Test NodeData URL\n";
    h += "  qos_st_get        : Test Status GET URL\n";
    h += "  qos_st_post       : Test Status POST URL\n";
    h += "  qos_fw            : Test Firmware URL\n";
#if GH_ID_CONFIG == 2
    h += "  qos_cam           : Test Camera API\n";
#endif

    h += "\n[ DATA SOURCE MODE ]\n";
    h += "  mode cloud        : Force use Cloud API\n";
    h += "  mode local        : Force use Local Average\n";
    h += "  mode auto         : Auto Failover Logic\n";

    h += "\n[ CLOUD UPLINK ]\n";
    h += "  uplink show       : Show direct/relay route status\n";
    h += "  uplink direct     : Force direct origin API\n";
    h += "  uplink relay      : Force relay API\n";
    h += "  uplink auto       : Direct first, relay fallback\n";

    h += "\n[ AUTHENTICATION ]\n";
    h += "  login <pass>      : Admin Login\n";

    if (webSerialAdmin)
    {
        h += "\n[ ADMIN - CONFIGURATION ]\n";
        h += "  settoken <str>    : Set API Token\n";
        h += "  seturldata <url>  : Set Node Data Base URL\n";
        h += "  seturlth <url>    : Set Threshold URL\n";
        h += "  setadminpass <pw> : Change Admin Password\n";
        h += "  get_now <id>      : Fetch data from Node ID\n";

        h += "\n[ ADMIN - SYSTEM ]\n";
        h += "  formatsdcard      : Format SD Card (Wipe)\n";
        h += "  resetwifi         : Clear WiFi & Reboot\n";
        h += "  reboot            : Restart Device\n";
        h += "  logout            : Admin Logout\n";
    }

    h += "=================================================\n";
    WebSerial.print(h);
}

void recvMsg(AsyncWebSocketClient* client, uint8_t *data, size_t len)
{
    String d = "";
    bool secureFrame = false;
    if (CryptoUtils::isNodeMediniEncryptedPayload(data, len))
    {
        char decryptedBuf[CryptoUtils::MAX_DECRYPTED_SIZE];
        size_t decryptedLen = 0;
        if (!CryptoUtils::decryptNodeMediniPayload(reinterpret_cast<const char*>(data),
                                                   len,
                                                   decryptedBuf,
                                                   sizeof(decryptedBuf),
                                                   &decryptedLen))
        {
            return;
        }
        d = String(decryptedBuf, decryptedLen);
        secureFrame = true;
    }
    else
    {
        for (size_t i = 0; i < len; i++)
            d += char(data[i]);
    }
    if (!secureFrame)
    {
        WebSerial.println("Encrypted frame required");
        return;
    }
    d.trim();
    String cmd = d;
    String arg = "";
    int spaceIndex = d.indexOf(' ');
    if (spaceIndex != -1)
    {
        cmd = d.substring(0, spaceIndex);
        arg = d.substring(spaceIndex + 1);
    }
    const bool redactInput = (cmd == "login" ||
                              cmd == "auth_token" ||
                              cmd == "settoken" ||
                              cmd == "setadminpass");
    WebSerial.println("> " + (redactInput ? (cmd + " [REDACTED]") : d));
    const bool webSerialAdmin = isWebSerialAdminClient(client);

    if (cmd == "help")
        printWebSerialHelp(webSerialAdmin);
    else if (cmd == "auth_token")
    {
        if (!secureFrame)
        {
            setWebSerialAdminClient(client, false);
            WebSerial.println("Encrypted frame required");
        }
        else if (isAdminSessionTokenValid(arg, getWebSocketClientBinding(client)))
        {
            setWebSerialAdminClient(client, true);
            WebSerial.println("Terminal auth OK");
        }
        else
        {
            setWebSerialAdminClient(client, false);
            WebSerial.println("Terminal auth failed");
        }
    }
    else if (cmd == "get_now")
    {
        if (!webSerialAdmin)
        {
            WebSerial.println("Auth Required");
        }
        else if (arg.length() > 0)
        {
            int targetId = arg.toInt();
            if (enqueueManualFetchMutation(targetId))
                WebSerial.printf("Queued manual fetch for GH ID: %d\n", targetId);
            else
                WebSerial.println("[BUSY] State update queue full.");
        }
        else
        {
            WebSerial.println("Usage: get_now <gh_id>");
        }
    }
    // --------------------------

    else if (cmd == "qos_cam")
    {
        if (!webSerialAdmin)
        {
            WebSerial.println("Auth Required");
        }
        else
        {
#if GH_ID_CONFIG == 2
            WebSerial.println("Testing Camera API...");
            net.qosCheck_Camera(WebSerial);
#else
            WebSerial.println("GH 2 Only");
#endif
        }
    }
    else if (cmd == "mode")
    {
        if (arg == "cloud")
        {
            if (enqueueSourceModeMutation(SOURCE_CLOUD))
                WebSerial.println("Queued source change: CLOUD");
            else
                WebSerial.println("[BUSY] State update queue full.");
        }
        else if (arg == "local")
        {
            if (enqueueSourceModeMutation(SOURCE_LOCAL))
                WebSerial.println("Queued source change: LOCAL");
            else
                WebSerial.println("[BUSY] State update queue full.");
        }
        else if (arg == "auto")
        {
            if (enqueueSourceModeMutation(SOURCE_AUTO))
                WebSerial.println("Queued source change: AUTO");
            else
                WebSerial.println("[BUSY] State update queue full.");
        }
        else
        {
            WebSerial.println("Usage: mode [cloud|local|auto]");
        }
    }
    else if (cmd == "uplink")
    {
        if (arg.length() == 0 || arg == "show")
        {
            char uplinkMode[8];
            char uplinkRoute[8];
            net.copyUplinkModeString(uplinkMode, sizeof(uplinkMode));
            net.copyActiveUplinkRouteString(uplinkRoute, sizeof(uplinkRoute));
            WebSerial.printf("Uplink Mode : %s\n", uplinkMode);
            WebSerial.printf("Active Route: %s\n", uplinkRoute);
        }
        else
        {
            if (!webSerialAdmin)
            {
                WebSerial.println("Auth Required");
                return;
            }
            MyNetworkManager::UplinkMode mode = MyNetworkManager::UplinkMode::AUTO;
            if (arg == "auto")
                mode = MyNetworkManager::UplinkMode::AUTO;
            else if (arg == "direct")
                mode = MyNetworkManager::UplinkMode::DIRECT;
            else if (arg == "relay")
                mode = MyNetworkManager::UplinkMode::RELAY;
            else
            {
                WebSerial.println("Usage: uplink [show|auto|direct|relay]");
                return;
            }

            net.setUplinkMode(mode);
            saveGatewayUplinkMode(mode);

            char uplinkMode[8];
            char uplinkRoute[8];
            net.copyUplinkModeString(uplinkMode, sizeof(uplinkMode));
            net.copyActiveUplinkRouteString(uplinkRoute, sizeof(uplinkRoute));
            WebSerial.printf("Uplink updated: %s (%s)\n", uplinkMode, uplinkRoute);
        }
    }
    else if (cmd == "status")
    {
        String s = "";
        s += "\n+---------------- SYSTEM STATUS ----------------+\n";

        char buf[64];
        snprintf(buf, sizeof(buf), "| Firmware : %-10s | GH ID : %d\n", FIRMWARE_VERSION, GH_ID_CONFIG);
        s += buf;
        snprintf(buf, sizeof(buf), "| Time     : %s\n", rtc_mgr.getTime());
        s += buf;

        s += "+-------------------- SENSORS ------------------+\n";
        snprintf(buf, sizeof(buf), "| Temp: %-5.1f | Hum: %-3.0f%% | Light: %-4.0f\n", sensorData.temperature, sensorData.humidity, sensorData.light);
        s += buf;

        s += "+-------------------- NETWORK ------------------+\n";
        snprintf(buf, sizeof(buf), "| State    : %-10s | Type  : %s\n", net.isConnected() ? "ONLINE" : "OFFLINE", net.isWiFiConnected() ? "WiFi" : "GPRS");
        s += buf;
        snprintf(buf, sizeof(buf), "| Mode     : %s\n", sensorData.getModeString().c_str());
        s += buf;
        const GatewayControlState controlState = getControlState(millis());
        snprintf(buf, sizeof(buf), "| Config   : %-10s | Runtime: %s\n",
                 sensorData.getConfiguredModeString().c_str(),
                 controlState.runtimeDataSource);
        s += buf;
        char uplinkMode[8];
        char uplinkRoute[8];
        net.copyUplinkModeString(uplinkMode, sizeof(uplinkMode));
        net.copyActiveUplinkRouteString(uplinkRoute, sizeof(uplinkRoute));
        snprintf(buf, sizeof(buf), "| Uplink   : %-10s | Route : %s\n", uplinkMode, uplinkRoute);
        s += buf;
        snprintf(buf, sizeof(buf), "| Sync     : %s\n",
                 cloudSyncModeToString(getCloudSyncMode(millis())));
        s += buf;

        s += "+-----------------------------------------------+\n";
        WebSerial.print(s);
    }
    else if (cmd == "login")
    {
        if (!secureFrame)
        {
            setWebSerialAdminClient(client, false);
            WebSerial.println("Encrypted frame required");
        }
        else
        {
            unsigned long retryAfterMs = 0;
            if (verifyAdminPasswordWithRateLimit(getWebSocketClientBinding(client), arg, &retryAfterMs))
            {
                setWebSerialAdminClient(client, true);
                WebSerial.println("Login OK");
            }
            else if (retryAfterMs > 0)
            {
                WebSerial.printf("Too many attempts. Retry in %lus\n", (retryAfterMs + 999UL) / 1000UL);
            }
            else
            {
                WebSerial.println("Wrong Pass");
            }
        }
    }
    else if (cmd == "memo_status")
    {
        WebSerial.printf("Heap: %u bytes (min %u)\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
#if defined(ESP32)
        WebSerial.printf("PSRAM: %u bytes (min %u)\n", ESP.getFreePsram(), ESP.getMinFreePsram());
#endif
    }
    else if (cmd == "qos_th")
    {
        if (!webSerialAdmin)
            WebSerial.println("Auth Required");
        else
        {
            WebSerial.println("Testing Threshold...");
            net.qosCheck_Threshold(WebSerial);
        }
    }
    else if (cmd == "qos_nd")
    {
        if (!webSerialAdmin)
            WebSerial.println("Auth Required");
        else
        {
            WebSerial.println("Testing NodeData...");
            net.qosCheck_NodeData(WebSerial);
        }
    }
    else if (cmd == "qos_st_get")
    {
        if (!webSerialAdmin)
            WebSerial.println("Auth Required");
        else
        {
            WebSerial.println("Testing Status GET...");
            net.qosCheck_DeviceStatusGet(WebSerial);
        }
    }
    else if (cmd == "qos_st_post")
    {
        if (!webSerialAdmin)
            WebSerial.println("Auth Required");
        else
        {
            WebSerial.println("Testing Status POST...");
            net.qosCheck_DeviceStatusPost(GH_ID_CONFIG, WebSerial);
        }
    }
    else if (cmd == "qos_fw")
    {
        if (!webSerialAdmin)
            WebSerial.println("Auth Required");
        else
        {
            WebSerial.println("Testing Firmware...");
            net.qosCheck_Firmware(WebSerial);
        }
    }
    else if (cmd == "sdcard_status")
    {
        if (sdCardOk)
        {
            SDCardInfo info = sd_logger.getStorageInfo();
            WebSerial.printf("SD: %.2f/%.2f MB\n", info.usedBytes / 1048576.0, info.totalBytes / 1048576.0);
        }
        else
            WebSerial.println("SD: Failed");
    }
    else if (cmd == "formatsdcard" && webSerialAdmin)
    {
        if (arg == "confirm")
        {
            if (sd_logger.formatLog())
                WebSerial.println("Formatted");
            else
                WebSerial.println("Fail");
        }
        else
            WebSerial.println("Type 'formatsdcard confirm'");
    }
    else if (cmd == "reboot" && webSerialAdmin)
    {
        WebSerial.println("Rebooting...");
        delay(500);
        ESP.restart();
    }
    else if (cmd == "resetwifi" && webSerialAdmin)
    {
        clearWiFiCredentials(lcd);
        ESP.restart();
    }
    else if (cmd == "logout" && webSerialAdmin)
    {
        setWebSerialAdminClient(client, false);
        WebSerial.println("Logged out");
    }
    else if (cmd == "settoken" && webSerialAdmin)
    {
        if (arg.length() == 0)
        {
            WebSerial.println("Usage: settoken <str>");
        }
        else
        {
            strncpy(active_api_token, arg.c_str(), sizeof(active_api_token) - 1);
            active_api_token[sizeof(active_api_token) - 1] = '\0';
            Preferences prefs; prefs.begin("device-config", false);
            prefs.putString("token", active_api_token);
            prefs.end();
            net.updateApiConfig(active_api_token, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            WebSerial.println("Token updated");
        }
    }
    else if (cmd == "seturldata" && webSerialAdmin)
    {
        if (arg.length() == 0)
        {
            WebSerial.println("Usage: seturldata <url>");
        }
        else
        {
            strncpy(active_nd_url_base, arg.c_str(), sizeof(active_nd_url_base) - 1);
            active_nd_url_base[sizeof(active_nd_url_base) - 1] = '\0';

            // Build active_nd_url safely (handle gh_id parameter if provided)
            String base = active_nd_url_base;
            int q = base.indexOf("gh_id=");
            if (q != -1)
            {
                int vStart = q + 6;
                int vEnd = base.indexOf('&', vStart);
                if (vEnd == -1) vEnd = base.length();
                base = base.substring(0, vStart) + String(GH_ID_CONFIG) + base.substring(vEnd);
                strncpy(active_nd_url, base.c_str(), sizeof(active_nd_url) - 1);
                active_nd_url[sizeof(active_nd_url) - 1] = '\0';
            }
            else
            {
                snprintf(active_nd_url, sizeof(active_nd_url), "%s%d", active_nd_url_base, GH_ID_CONFIG);
            }

            Preferences prefs; prefs.begin("device-config", false);
            prefs.putString("nd_url_base", active_nd_url_base);
            prefs.end();

            net.updateApiConfig(nullptr, nullptr, nullptr, active_nd_url, active_nd_url_base, nullptr, nullptr, nullptr);
            WebSerial.println("Node Data URL updated");
        }
    }
    else if (cmd == "seturlth" && webSerialAdmin)
    {
        if (arg.length() == 0)
        {
            WebSerial.println("Usage: seturlth <url>");
        }
        else
        {
            strncpy(active_th_url, arg.c_str(), sizeof(active_th_url) - 1);
            active_th_url[sizeof(active_th_url) - 1] = '\0';
            Preferences prefs; prefs.begin("device-config", false);
            prefs.putString("th_url", active_th_url);
            prefs.end();
            net.updateApiConfig(nullptr, nullptr, active_th_url, nullptr, nullptr, nullptr, nullptr, nullptr);
            WebSerial.println("Threshold URL updated");
        }
    }
    else if (cmd == "setadminpass" && webSerialAdmin)
    {
        if (arg.length() == 0)
        {
            WebSerial.println("Usage: setadminpass <pw>");
        }
        else
        {
            strncpy(active_admin_pass, arg.c_str(), sizeof(active_admin_pass) - 1);
            active_admin_pass[sizeof(active_admin_pass) - 1] = '\0';
            Preferences prefs; prefs.begin("device-config", false);
            prefs.putString("admin_pass", active_admin_pass);
            prefs.end();
            invalidateAdminSessionToken();
            clearAllWebSerialAdminSessions();
            WebSerial.println("Admin password updated");
        }
    }
    else
        WebSerial.println("Unknown/Auth Required");
}

class LogDownloadResponse : public AsyncFileResponse
{
public:
    LogDownloadResponse(fs::FS &fs, const String &path, const String &contentType, bool download, SDCardLogger &logger)
        : AsyncFileResponse(fs, path, contentType, download), _logger(logger)
    {

        // Kunci SD Card saat objek respons dibuat
        _logger.setBusy(true);
        esp_task_wdt_reset();
        Serial.println(F("[SD] Download started. Logging paused."));
    }

    ~LogDownloadResponse()
    {
        // Buka kunci SD Card saat objek respons dihancurkan (selesai/putus)
        _logger.setBusy(false);
        Serial.println(F("[SD] Download finished/aborted. Logging resumed."));
    }

private:
    SDCardLogger &_logger;
};
// ==================================================================================
//   SETUP
// ==================================================================================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 2000)
        ;
    Serial.println(F("\n--- ESP32 Controller ---"));

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdt = {.timeout_ms = WDT_TIMEOUT * 1000, .idle_core_mask = 0, .trigger_panic = true};
    esp_task_wdt_reconfigure(&wdt);
#else
    esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
    esp_task_wdt_add(NULL);

    Wire.begin(SDA_PIN, SCL_PIN);
    lcd.begin();
    sensorData.begin();
    loadConfiguration();

#ifdef SCHEDULE_URL
    strncpy_P(active_schedule_url, SCHEDULE_URL, sizeof(active_schedule_url) - 1);
#else
    strcpy(active_schedule_url, "");
#endif

    sd_logger.begin();
    relay.begin();
    relay.loadOverrides();

    char apn[32], usr[32], pass[32], pin[10];
    strncpy_P(apn, GPRS_APN, 31);
    strncpy_P(usr, GPRS_USER, 31);
    strncpy_P(pass, GPRS_PASSWORD, 31);
    strncpy_P(pin, SIM_PIN, 9);

    net.begin(active_ssid, active_pwd, active_api_token, active_ta_token, active_th_url, active_nd_url, active_nd_url_base,
              active_device_status_post_url, active_device_status_get_url, active_schedule_url, apn, usr, pass, pin);
    net.setUplinkMode(loadGatewayUplinkMode());

    const bool connectedDuringStartup = waitForInitialNetworkConnection(kInitialNetworkConnectWindowMs);
    if (!connectedDuringStartup)
        startConfigPortal(lcd);

    WiFi.setSleep(false);
    g_mdnsHost = "gateway-gh-" + String(GH_ID_CONFIG);
    ensureMdnsService();

    // API Mode
    server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest *r)
              {
            int m = getControlState(millis()).apiModeValue;
            r->send(200, "application/json", "{\"mode\":"+String(m)+"}"); });

    server.on("/api/time", HTTP_GET, [](AsyncWebServerRequest *r)
              {
            rtc_mgr.update();
            JsonDocument doc;
            const GatewayControlState controlState = getControlState(millis());
            const bool scheduleRequired = controlState.runtimeScheduleConfigured;
            const bool scheduleReady = controlState.runtimeScheduleReady;
            doc["time_valid"] = rtc_mgr.isTimeSet();
            doc["epoch_utc"] = time(nullptr);
            doc["local_time"] = rtc_mgr.getTime();
            doc["timezone_name"] = rtc_mgr.getConfiguredTimezoneName();
            doc["timezone_posix"] = rtc_mgr.getConfiguredTimezonePosix();
            doc["expected_utc_offset"] = rtc_mgr.getConfiguredUtcOffset();
            doc["last_sync_source"] = rtc_mgr.getLastSyncSourceString();
            doc["last_sync_epoch_utc"] = rtc_mgr.getLastSyncEpochUtc();
            doc["last_sync_millis"] = rtc_mgr.getLastSyncMillis();
            doc["last_sync_authoritative"] = rtc_mgr.wasLastSyncAuthoritative();
            doc["last_validated_timezone"] = rtc_mgr.getLastValidatedTimezone();
            doc["last_validated_utc_offset"] = rtc_mgr.getLastValidatedUtcOffset();
            doc["ntp_primary"] = rtc_mgr.getPrimaryNtpServer();
            doc["ntp_secondary"] = rtc_mgr.getSecondaryNtpServer();
            doc["ntp_tertiary"] = rtc_mgr.getTertiaryNtpServer();
            doc["last_ntp_server"] = rtc_mgr.getLastNtpServer();
            doc["time_api_url"] = net.getWorldTimeUrl();
            doc["schedule_required"] = scheduleRequired;
            doc["schedule_ready"] = scheduleReady;
            doc["schedule_source"] = controlState.scheduleEffectiveSource;
            doc["runtime_source"] = controlState.runtimeDataSource;
            doc["mode"] = sensorData.getModeString();
            String payload;
            serializeJson(doc, payload);
            r->send(200, "application/json", payload); });

    server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request)
              {
            if (!isAdminRequestAuthorized(request, true)) {
                request->send(403, "text/plain", "Forbidden");
                return;
            }
            if (request->hasParam("file")) {
                String fileName = request->getParam("file")->value();
                if (fileName != "/log.csv" && fileName != "/qos.csv") {
                    request->send(403, "text/plain", "Forbidden");
                    return;
                }
                sd_logger.closeFiles(); // Tutup dulu filenya agar bisa diakses browser
                delay(200);   
                
                if (SD.exists(fileName)) {
                    // 1. Ambil ukuran file untuk memastikan tidak 0 bytes
                    File testFile = SD.open(fileName, FILE_READ);
                    size_t fileSize = testFile.size();
                    testFile.close();

                    if (fileSize == 0) {
                        request->send(200, "text/plain", "File exists but is empty.");
                        return;
                    }

                    // 2. TUTUP file di logger agar tidak "Locked"
                    sd_logger.closeFiles(); 
                    delay(100); // Beri jeda sistem

                    // 3. Kirim file
                    esp_task_wdt_reset();
                    LogDownloadResponse *response = new LogDownloadResponse(SD, fileName, "application/octet-stream", true, sd_logger);
                    response->addHeader("Cache-Control", "no-cache");
                    request->send(response);
                } else {
                    request->send(404, "text/plain", "File Not Found");
                    sd_logger.reInit(); // Buka kembali file log jika gagal
                }
            } else {
                request->send(400, "text/plain", "Bad Request");
            } });

    // API Data Handler
    server.on("/api/data", HTTP_POST, [](AsyncWebServerRequest *r) {}, NULL, [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t idx, size_t total)
        {
            constexpr size_t kMaxPayloadBytes = 4096;

            if (idx == 0) {
                cleanupApiDataUploadContext(r);
                if (total == 0 || total > kMaxPayloadBytes) {
                    r->send(413, "application/json", "{\"error\":\"Payload too large\"}");
                    return;
                }

                auto *ctx = new (std::nothrow) ApiDataUploadContext();
                if (!ctx) {
                    r->send(503, "application/json", "{\"error\":\"No memory\"}");
                    return;
                }

                ctx->buffer = new (std::nothrow) char[total + 1];
                if (!ctx->buffer) {
                    delete ctx;
                    r->send(503, "application/json", "{\"error\":\"No memory\"}");
                    return;
                }

                ctx->total = total;
                ctx->received = 0;
                r->_tempObject = ctx;
            }

            auto *ctx = static_cast<ApiDataUploadContext *>(r->_tempObject);
            if (!ctx || !ctx->buffer) {
                cleanupApiDataUploadContext(r);
                r->send(500, "application/json", "{\"error\":\"Upload state lost\"}");
                return;
            }

            if ((idx + len) > ctx->total) {
                cleanupApiDataUploadContext(r);
                r->send(400, "application/json", "{\"error\":\"Invalid chunk\"}");
                return;
            }

            memcpy(ctx->buffer + idx, data, len);
            ctx->received += len;

            if ((idx + len) < total) {
                return;
            }

            if (ctx->received != ctx->total) {
                cleanupApiDataUploadContext(r);
                r->send(400, "application/json", "{\"error\":\"Incomplete body\"}");
                return;
            }

            ctx->buffer[ctx->total] = '\0';

            const uint8_t *rawPayload = reinterpret_cast<const uint8_t *>(ctx->buffer);
            const size_t payloadLen = ctx->total;
            const size_t pSize = payloadLen;
            const unsigned long rxEpoch = time(nullptr);

            char decryptBuf[CryptoUtils::MAX_DECRYPTED_SIZE];
            const char *json = ctx->buffer;
            size_t jLen = payloadLen;
            if (CryptoUtils::isEncryptedPayload(rawPayload, payloadLen)) {
                size_t dLen = 0;
                if (CryptoUtils::decryptPayload(ctx->buffer, payloadLen, decryptBuf, sizeof(decryptBuf), &dLen)) {
                    json = decryptBuf;
                    jLen = dLen;
                } else {
                    cleanupApiDataUploadContext(r);
                    r->send(400, "application/json", "{\"error\":\"Decrypt Fail\"}");
                    return;
                }
            }

            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, json, jLen);
            if (err) {
                cleanupApiDataUploadContext(r);
                r->send(400, "application/json", "{\"error\":\"Bad JSON\"}"); 
                return;
            }

            // Ambil Data sesuai JSON dari Node
            int received_gh_id = doc["gh_id"] | 0;
            
            String nodeName;
            if (doc["node_id"].is<int>()) {
                nodeName = "node-" + doc["node_id"].as<String>();
            } else {
                nodeName = doc["node_id"].as<String>();
            }

            const unsigned long tx = extractTxEpoch(doc);
            int rssi_active = doc["rssi"] | 0;
            int rssi_nonactive = doc["rssi_nonactive"] | 0;

            // --- FILTER: APAKAH INI NODE SENDIRI ATAU NODE NYASAR? ---
            if (received_gh_id == GH_ID_CONFIG) {
                bool queueOk = false;

                // KONDISI 1: NODE NORMAL (Milik GH ini)
                if (nodeName.startsWith("cam")) {
                    bool foggy = false;
                    const JsonVariantConst fogValue = doc["is_foggy"];
                    bool fogParsed = false;
                    if (fogValue.is<bool>()) {
                        foggy = fogValue.as<bool>();
                        fogParsed = true;
                    } else if (fogValue.is<int>()) {
                        const int fogInt = fogValue.as<int>();
                        if (fogInt == 0 || fogInt == 1) {
                            foggy = (fogInt == 1);
                            fogParsed = true;
                        }
                    } else if (fogValue.is<const char*>()) {
                        const char* rawFog = fogValue.as<const char*>();
                        if (rawFog) {
                            if (strcmp(rawFog, "1") == 0 ||
                                strcmp(rawFog, "true") == 0 ||
                                strcmp(rawFog, "TRUE") == 0 ||
                                strcmp(rawFog, "True") == 0) {
                                foggy = true;
                                fogParsed = true;
                            } else if (strcmp(rawFog, "0") == 0 ||
                                       strcmp(rawFog, "false") == 0 ||
                                       strcmp(rawFog, "FALSE") == 0 ||
                                       strcmp(rawFog, "False") == 0) {
                                foggy = false;
                                fogParsed = true;
                            }
                        }
                    }
                    if (!fogParsed) {
                        cleanupApiDataUploadContext(r);
                        r->send(422, "application/json", "{\"error\":\"Invalid camera fog payload\"}");
                        return;
                    }
                    float conf = doc["confidence"] | 0.0;
                    queueOk = enqueueNodeMutation(nodeName.c_str(), NAN, NAN, NAN, foggy, conf, tx, rxEpoch, pSize, false);
                } else {
                    float t = NAN;
                    float h = NAN;
                    float l = NAN;
                    if (!parseLocalSensorPayload(doc, t, h, l)) {
                        cleanupApiDataUploadContext(r);
                        r->send(422, "application/json", "{\"error\":\"Invalid sensor payload\"}");
                        return;
                    }

                    queueOk = enqueueNodeMutation(nodeName.c_str(), t, h, l, false, 0, tx, rxEpoch, pSize, true);
                }
                if (!queueOk) {
                    cleanupApiDataUploadContext(r);
                    r->send(422, "application/json", "{\"error\":\"Invalid node update\"}");
                    return;
                }

            } else {
                
                // KONDISI 2: NODE NYASAR / CROSS-TALK (Dari GH sebelah)
                String msg = "[CROSS-TALK] " + nodeName + " (Asli GH " + String(received_gh_id) + 
                             ") lompat ke Gateway GH " + String(GH_ID_CONFIG) + 
                             " | RSSI Act: " + String(rssi_active) + " | NonAct: " + String(rssi_nonactive);
                
                // Tampilkan di WebSerial & Serial Monitor
                WebSerial.println(msg); 
                Serial.println(msg);
                
                // Data JANGAN dimasukkan ke sensorData.updateFromNode agar rata-rata suhu lokal tidak rusak!
            }

            // --- QOS LOGGING UNTUK SEMUA NODE (TERMASUK YANG NYASAR) ---
            // [PERBAIKAN GH 2]: Syarat rtc_mgr.isHardwareOk() DIHAPUS agar SD Card GH2 tetap menyimpan data!
            if(sdCardOk && tx > 0) {
                if (!enqueueQosLogMutation(nodeName.c_str(), tx, pSize, rssi_active, rssi_nonactive)) {
                    static unsigned long lastQosQueueWarnMs = 0;
                    const unsigned long qosWarnNow = millis();
                    if (qosWarnNow - lastQosQueueWarnMs >= 10000UL) {
                        lastQosQueueWarnMs = qosWarnNow;
                        const String warnMsg = "[WARN] QoS log queue full. Dropping log entries until queue drains.";
                        WebSerial.println(warnMsg);
                        Serial.println(warnMsg);
                    }
                }
            }

            cleanupApiDataUploadContext(r);
            r->send(200, "application/json", "{\"status\":\"ok\"}"); 
        });

    server.on("/api/relays/control", HTTP_POST, [](AsyncWebServerRequest *r)
              {
            r->send(403, "application/json", "{\"error\":\"Manual override disabled; control uses threshold and schedule only\"}"); });

    WebSerial.begin(&server);
    WebSerial.onMessage(recvMsg);
    setupOtaHandlers(server, lcd);
    wsManager.begin(&server);
    server.begin();

    rtc_mgr.begin();
    if (net.isConnected())
        pendingRtcDriftCheck = true;
    if (sdCardOk)
        sensorData.loadFromLog();
    if (sensorData.getUsableLocalNodeCount() > 0)
    {
        lastLocalNodeUpdate = millis();
        sensorData.noteLocalSensorUpdate(lastLocalNodeUpdate);
    }
    updateEffectiveDataSource(millis());
    const GatewayControlState startupControlState = getControlState(millis());
    if (sensorData.currentMode == SOURCE_AUTO)
    {
        if (startupControlState.localControlHealthy || startupControlState.cloudControlHealthy)
            lastHealthyControlPathMs = millis();
    }
    else if (!startupControlState.activeSourceStale)
    {
        lastHealthyControlPathMs = millis();
    }
    lastLoop = millis();
    lastApiAttempt = millis() - API_MS;
    lastScheduleFetch = millis() - API_MS;
    lcd.message(0, 0, "Setup OK", true);
    delay(1000);
}

// ==================================================================================
//   LOOP
// ==================================================================================
void loop()
{
    esp_task_wdt_reset();
    processPendingControlActions();
    unsigned long now = millis();
    runControlLoopIfDue(now);

    const bool wasConnected = net.isConnected();
    net.handleWiFi(computeControlSafeBlockingBudget(now));
    wsManager.pumpAsyncEvents();
    now = millis();
    const bool isConnectedNow = net.isConnected();
    if (isConnectedNow)
    {
        lastNetworkAvailable = now;
        if (!rtc_mgr.isTimeSet())
            pendingRtcDriftCheck = true;
        if (!wasConnected)
        {
            currentRetryDelay = INITIAL_RETRY_DELAY_MS;
            pendingRtcDriftCheck = true;
        }
    }
    else if (now - lastConnectionRetry >= 10000UL)
    {
        lastConnectionRetry = now;
        lcd.message(0, 3, "Reconnect...", true);
    }

    // 1. Logika Re-Koneksi Network
    const MyNetworkManager::WiFiState netState = net.getWiFiState();
    const bool gprsRecoveryActive =
        netState == MyNetworkManager::WiFiState::FALLBACK_GPRS_WAIT ||
        netState == MyNetworkManager::WiFiState::FALLBACK_GPRS_MODEM_INIT ||
        netState == MyNetworkManager::WiFiState::FALLBACK_GPRS_SIM_READY ||
        netState == MyNetworkManager::WiFiState::FALLBACK_GPRS_NETWORK_ATTACH ||
        netState == MyNetworkManager::WiFiState::FALLBACK_GPRS_CONNECTING;
    if (!net.isWiFiConnected() &&
        (net.isGprsConnected() || gprsRecoveryActive) &&
        (now - lastWiFiRetryWhenGprs >= WIFI_RETRY_WHEN_GPRS_MS))
    {
        lastWiFiRetryWhenGprs = now;
        net.triggerRescan();
    }
    const bool deferredNetworkAttempted = processDeferredNetworkActions();
    const bool maintenanceAttempted = deferredNetworkAttempted ? false : serviceDeferredMaintenance(millis());
    const bool blockingNetworkConsumedThisLoop = deferredNetworkAttempted || maintenanceAttempted;
    now = millis();

    ensureMdnsService();
    updateEffectiveDataSource(now);

    // 2. Logika Pengambilan Data API (Cloud)
    const CloudSyncMode cloudSyncMode = getCloudSyncMode(now);
    const bool fetchTrigger = shouldTriggerCloudFetch(now);

    if (fetchTrigger && apiStep == 0)
    {
        apiSyncMode = cloudSyncMode;
        apiRecoveryNodeOk = false;
        apiRecoveryThresholdOk = false;
        apiRecoveryScheduleOk = false;
        apiStep = 1; // Mulai urutan pengambilan data
    }

    bool apiRequestAttemptedThisLoop = false;
    if (apiStep > 0 && !blockingNetworkConsumedThisLoop)
    {
        unsigned long currentTime = millis();

        // Step 1: Ambil Node Data (Data Utama)
        if (apiStep == 1)
        {
            serviceCriticalControlPath();
            currentTime = millis();
            if (apiStep != 1)
                goto api_step_rechecked;
            const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(currentTime);
            if (requestTimeoutMs == 0)
                goto api_step_rechecked;
            lastApiAttempt = currentTime; // Reset timer loop utama
            apiRequestAttemptedThisLoop = true;
            CloudSensorSnapshot sensorSnapshot = {};
            const bool sensorFetchOk = net.fetchNodeData(sensorSnapshot, requestTimeoutMs);
            if (sensorFetchOk)
                applyCloudSensorSnapshot(sensorSnapshot);
            const bool cloudDataOk = sensorFetchOk &&
                                     isCloudControlSensorSnapshotReady(sensorSnapshot);
            if (cloudDataOk)
            {
                sensorData.noteCloudSensorSuccess(currentTime);
                isInFailSafeMode = false;
                if (apiSyncMode == CLOUD_SYNC_RECOVERY)
                {
                    apiRecoveryNodeOk = true;
                }
                else
                {
                    sensorData.onCloudSuccess();
                }
                apiStep = 2; // Lanjut ke step berikutnya
                lastStepMillis = currentTime;
            }
            else
            {
                if (sensorFetchOk)
                {
                    Serial.printf("[ND] Cloud control payload incomplete; missing/invalid: %s\n",
                                  describeCloudControlSensorGap(sensorSnapshot).c_str());
                }
                sensorData.onCloudFailure();
                wsManager.broadcastStatus();
                apiStep = 0; // Gagal, stop urutan
            }
        }
        // Step 2: Tunggu 3 detik tanpa memblokir sistem, lalu ambil Threshold
        else if (apiStep == 2 && (currentTime - lastStepMillis >= 3000))
        {
            serviceCriticalControlPath();
            currentTime = millis();
            if (apiStep != 2)
                goto api_step_rechecked;
            const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(currentTime);
            if (requestTimeoutMs == 0)
                goto api_step_rechecked;
            apiRequestAttemptedThisLoop = true;
            CloudThresholdSnapshot thresholdSnapshot = {};
            const bool thresholdOk = net.fetchThresholds(thresholdSnapshot, requestTimeoutMs);
            if (thresholdOk)
                applyCloudThresholdSnapshot(thresholdSnapshot);
            apiRecoveryThresholdOk = thresholdOk;
            if (apiSyncMode == CLOUD_SYNC_RECOVERY && !thresholdOk)
            {
                sensorData.onCloudFailure();
                wsManager.broadcastStatus();
                apiStep = 0;
            }
            else if (apiSyncMode == CLOUD_SYNC_RECOVERY)
            {
                apiStep = 3;
            }
            else
            {
#if GH_ID_CONFIG == 2
                apiStep = 4;
#else
                apiStep = 0;
                wsManager.broadcastStatus();
#endif
            }
            lastStepMillis = currentTime;
        }
        // Step 3: Recovery probe ikut refresh schedule cloud agar bundle lengkap.
        else if (apiStep == 3 && (currentTime - lastStepMillis >= 3000))
        {
            serviceCriticalControlPath();
            currentTime = millis();
            if (apiStep != 3)
                goto api_step_rechecked;
            const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(currentTime);
            if (requestTimeoutMs == 0)
                goto api_step_rechecked;
            apiRequestAttemptedThisLoop = true;
            CloudScheduleSnapshot scheduleSnapshot = {};
            apiRecoveryScheduleOk = net.fetchSchedules(scheduleSnapshot, requestTimeoutMs);
            if (apiRecoveryScheduleOk)
            {
                applyCloudScheduleSnapshot(scheduleSnapshot);
                lastScheduleFetch = currentTime;
#if GH_ID_CONFIG != 2
                if (apiRecoveryNodeOk && apiRecoveryThresholdOk)
                    sensorData.onCloudSuccess();
#else
                apiStep = 4;
                lastStepMillis = currentTime;
#endif
            }
            else
            {
                sensorData.onCloudFailure();
                apiStep = 0;
                wsManager.broadcastStatus();
            }
#if GH_ID_CONFIG != 2
            if (apiRecoveryScheduleOk)
            {
                apiStep = 0;
                wsManager.broadcastStatus();
            }
#else
#endif
        }
        // Step 4: Tunggu 3 detik, lalu ambil Status Kamera (Hanya GH2)
        else if (apiStep == 4 && (currentTime - lastStepMillis >= 3000))
        {
            serviceCriticalControlPath();
            currentTime = millis();
            if (apiStep != 4)
                goto api_step_rechecked;
#if GH_ID_CONFIG == 2
            const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(currentTime);
            if (requestTimeoutMs == 0)
                goto api_step_rechecked;
            apiRequestAttemptedThisLoop = true;
            CloudFogSnapshot fogSnapshot = {};
            const bool cameraOk = net.fetchCameraStatus(fogSnapshot, requestTimeoutMs);
            if (cameraOk)
                applyCloudFogSnapshot(fogSnapshot);
            if (apiSyncMode == CLOUD_SYNC_RECOVERY)
            {
                if (cameraOk && apiRecoveryNodeOk && apiRecoveryThresholdOk && apiRecoveryScheduleOk)
                    sensorData.onCloudSuccess();
                else
                    sensorData.onCloudFailure();
            }
#endif
            apiStep = 0;                 // Selesai semua urutan
            wsManager.broadcastStatus(); // Update dashboard
        }
api_step_rechecked:
        ;
    }

    if (!blockingNetworkConsumedThisLoop &&
        !apiRequestAttemptedThisLoop &&
        cloudSyncMode == CLOUD_SYNC_ACTIVE &&
        apiStep == 0)
    {
        if (now - lastScheduleFetch >= API_MS)
        {
            serviceCriticalControlPath();
            now = millis();
            if (apiStep == 0 && getCloudSyncMode(now) == CLOUD_SYNC_ACTIVE)
            {
                const uint16_t requestTimeoutMs = computeControlSafeHttpTimeout(now);
                if (requestTimeoutMs == 0)
                    goto skip_schedule_refresh;
                lastScheduleFetch = now;
                CloudScheduleSnapshot scheduleSnapshot = {};
                if (net.fetchSchedules(scheduleSnapshot, requestTimeoutMs))
                {
                    applyCloudScheduleSnapshot(scheduleSnapshot);
                    wsManager.broadcastStatus();
                }
            }
        }
    }
skip_schedule_refresh:

    updateEffectiveDataSource(now);
    const GatewayControlState controlState = getControlState(now);
    if (sensorData.currentMode == SOURCE_AUTO)
    {
        if (controlState.localControlHealthy ||
            controlState.cloudControlHealthy ||
            controlState.scheduleOnlyFallbackHealthy)
            lastHealthyControlPathMs = now;
    }
    else if (!controlState.activeSourceStale || controlState.scheduleOnlyFallbackHealthy)
    {
        lastHealthyControlPathMs = now;
    }

    // 3. Deteksi Failsafe
    bool shouldFailSafe = resolveShouldEnterFailSafe(controlState,
                                                     lastHealthyControlPathMs,
                                                     now);
    if (isInFailSafeMode && !shouldFailSafe)
    {
        isInFailSafeMode = false;
    }
    if (shouldFailSafe && !isInFailSafeMode)
    {
        isInFailSafeMode = true;
        relay.forceSafeState();
    }

    // 4. Eksekusi Kontrol Relay & Update Tampilan
    if (now - lastLoop >= LOOP_MS)
    {
        lastLoop = now;
        runControlLogic();
    }
    processPendingQosLogs();

    // 5. Broadcast Status ke Dashboard Web (WebSocket)
    if (net.isWiFiConnected() && (now - lastWsBroadcast >= WEB_UPDATE_INTERVAL_MS))
    {
        lastWsBroadcast = now;
        wsManager.broadcastStatus();
    }

    // 6. Maintenance (SD Card & RTC Sync)
    if (!sd_logger.isBusy() && !sdCardOk && (now - lastSdRetry >= SD_RETRY_INTERVAL_MS))
    {
        lastSdRetry = now;
        sd_logger.reInit();
    }

    if (rtc_mgr.isTimeSet() && net.isConnected() && (now - lastTimeSync >= TIME_SYNC_INTERVAL))
    {
        pendingRtcDriftCheck = true;
    }

    // 7. Cek Update Firmware OTA Otomatis (1 jam sekali)
    if (!sd_logger.isBusy() && net.isWiFiConnected() && (now - lastUpdateCheck >= 3600000UL))
    {
        lastUpdateCheck = now;
        pendingOtaCheck = true;
    }

    yield();
}

void runControlLogic()
{
    rtc_mgr.update();
    const unsigned long nowMs = millis();
    const GatewayControlState controlState = getControlState(nowMs);
    int rssi = net.getSignalQuality();
    const char* r1Decision = "N/A";
    const char* r2Decision = "N/A";
    const char* r3Decision = "N/A";
    bool r1ScheduleActive = false;
    bool r2ScheduleActive = false;
    bool r3ScheduleActive = false;
    int r1ScheduleId = -1;
    int r2ScheduleId = -1;
    int r3ScheduleId = -1;
    const bool useLocalSchedules = controlState.runtimeUsesLocalData;
    const float controlHumidity = sensorData.humidity;
    const float controlTemperature = sensorData.temperature;
    const float controlLight = sensorData.light;
    const bool controlFog = sensorData.isFoggy;
    const float controlTempMin = sensorData.getTempMin();
    const float controlTempMax = sensorData.getTempMax();
    const float controlHumMin = sensorData.getHumMin();
    const float controlHumMax = sensorData.getHumMax();
    const String gatewayMode = sensorData.getModeString();
    const char* thresholdSource = controlState.thresholdRuntimeSource;
    const bool timeValid = rtc_mgr.isTimeSet();
    const bool scheduleRuntimeReady = controlState.runtimeScheduleReady;
    const char* scheduleSource = controlState.scheduleEffectiveSource;
    const bool activeSourceHealthy = controlState.activeSourceHealthy;

    if (isInFailSafeMode)
    {
        relay.forceSafeState();
        r1Decision = "FAILSAFE";
        r2Decision = "FAILSAFE";
        r3Decision = "FAILSAFE";
    }
    else
    {
        int H = 0;
        int M = 0;
        if (timeValid)
        {
            time_t raw = time(nullptr);
            struct tm *ti = localtime(&raw);
            if (ti)
            {
                H = ti->tm_hour;
                M = ti->tm_min;
            }
        }
        const bool allowThresholdEvaluation = activeSourceHealthy;
        bool r1Changed = relay.updateSingleRelayState(RELAY_EXHAUST, controlHumidity, controlHumMin, controlHumMax, controlTemperature, controlTempMin, controlTempMax, H, M, controlFog, useLocalSchedules, timeValid, allowThresholdEvaluation);
        bool r2Changed = relay.updateSingleRelayState(RELAY_DEHUMIDIFIER, controlHumidity, controlHumMin, controlHumMax, controlTemperature, controlTempMin, controlTempMax, H, M, controlFog, useLocalSchedules, timeValid, allowThresholdEvaluation);
        bool r3Changed = relay.updateSingleRelayState(RELAY_BLOWER, controlTemperature, controlTempMin, controlTempMax, controlTemperature, controlTempMin, controlTempMax, H, M, controlFog, useLocalSchedules, timeValid, allowThresholdEvaluation);
        relay.ensureRelay4Off();

        r1Decision = relay.getRelayDecisionSourceString(RELAY_EXHAUST);
        r2Decision = relay.getRelayDecisionSourceString(RELAY_DEHUMIDIFIER);
        r3Decision = relay.getRelayDecisionSourceString(RELAY_BLOWER);
        r1ScheduleActive = relay.wasScheduleActiveForRelay(RELAY_EXHAUST);
        r2ScheduleActive = relay.wasScheduleActiveForRelay(RELAY_DEHUMIDIFIER);
        r3ScheduleActive = relay.wasScheduleActiveForRelay(RELAY_BLOWER);
        r1ScheduleId = relay.getActiveScheduleIdForRelay(RELAY_EXHAUST);
        r2ScheduleId = relay.getActiveScheduleIdForRelay(RELAY_DEHUMIDIFIER);
        r3ScheduleId = relay.getActiveScheduleIdForRelay(RELAY_BLOWER);

        if (r1Changed)
            queueRelayStatusSync(RELAY_EXHAUST, relay.getR1());
        if (r2Changed)
            queueRelayStatusSync(RELAY_DEHUMIDIFIER, relay.getR2());
        if (r3Changed)
            queueRelayStatusSync(RELAY_BLOWER, relay.getR3());
    }

    {
        static unsigned long lastStatusResync = 0;
        if (nowMs - lastStatusResync >= DEVICE_STATUS_RESYNC_INTERVAL_MS)
        {
            lastStatusResync = nowMs;
            queueRelayStatusResyncAll();
        }
    }
    bool stale = controlState.activeSourceStale;
    lcd.update(rtc_mgr.getTime(), controlTemperature, controlHumidity, controlLight,
               relay.getR1(), relay.getR2(), relay.getR3(), controlFog,
               controlTempMin, controlTempMax, controlHumMin, controlHumMax,
               net.isConnected(), stale, sdCardOk, isInFailSafeMode, rssi, net.isGprsConnected());

#if GH_ID_CONFIG == 2
    // Khusus GH 2: Abaikan validasi waktu, paksa simpan
    if (sdCardOk)
#else
    // GH 1 (atau lainnya): Tetap butuh waktu valid agar log rapi
    if (sdCardOk && rtc_mgr.getTime()[0] != 'Y')
#endif
    {
        sd_logger.logData(rtc_mgr.getTime(), controlTemperature, controlHumidity, controlLight,
                          rssi, net.isGprsConnected(),
                          relay.getR1(), relay.getR2(), relay.getR3(), relay.getR4(), controlFog,
                          controlTempMin, controlTempMax, controlHumMin, controlHumMax,
                          gatewayMode.c_str(), thresholdSource, scheduleSource,
                          r1ScheduleActive, r2ScheduleActive, r3ScheduleActive,
                          r1ScheduleId, r2ScheduleId, r3ScheduleId,
                          r1Decision, r2Decision, r3Decision);
    }
}

void checkForUpdates(uint16_t timeoutMs, bool allowApply)
{
    if (!net.isWiFiConnected())
        return;
    lcd.message(0, 3, "Chk OTA...", true);
    WiFiClientSecure c;
    c.setInsecure();
    c.setTimeout(timeoutMs);
    HTTPClient h;
    String otaUrl = net.resolveUplinkUrl(String(FW_UPDATE_URL) + String(FW_VERSION_ID));
    h.begin(c, otaUrl);
    h.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    h.setTimeout(timeoutMs);
    char userAgent[kGatewayUserAgentLen];
    buildGatewayUserAgent(userAgent, sizeof(userAgent));
    char deviceId[kGatewayDeviceIdLen];
    buildGatewayDeviceId(deviceId, sizeof(deviceId));
    const String gatewayUserAgent(userAgent);
    const String gatewayDeviceId(deviceId);
    h.setUserAgent(userAgent);
    h.addHeader("X-Device-ID", deviceId);
    if (h.GET() == 200)
    {
        JsonDocument d;
        DeserializationError err = deserializeJson(d, h.getStream());
        if (!err)
        {
            int status = d["status"] | 0;
            const char* fileUrl = d["file_url"] | "";
            const char* version = d["version"] | "";
            if (status == 1 && fileUrl[0] != '\0' && version[0] != '\0')
            {
                if (compareVersionStrings(String(version), String(FIRMWARE_VERSION)) > 0)
                {
                    if (!allowApply)
                    {
                        Serial.printf("[OTA] Update available: current=%s latest=%s auto-apply=OFF\n",
                                      FIRMWARE_VERSION,
                                      version);
                        WebSerial.printf("[OTA] Update available: %s -> %s (auto apply disabled)\n",
                                         FIRMWARE_VERSION,
                                         version);
                    }
                    else
                    {
                        lcd.message(0, 3, "Upd...", true);
                        HTTPUpdate hu;
                        hu.update(c, String(fileUrl), String(FIRMWARE_VERSION), [gatewayUserAgent, gatewayDeviceId](HTTPClient *http) {
                            if (http) {
                                http->setUserAgent(gatewayUserAgent);
                                http->addHeader("X-Device-ID", gatewayDeviceId);
                            }
                        });
                    }
                }
            }
        }
    }
    h.end();
}
