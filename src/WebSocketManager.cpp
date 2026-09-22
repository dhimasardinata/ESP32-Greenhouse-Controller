#include "WebSocketManager.h"
#include "SensorDataManager.h"
#include "RelayController.h"
#include "MyNetworkManager.h"
#include "RTCManager.h"
#include "ConfigManager.h"
#include "CryptoUtils.h"
#include "DeferredControlActions.h"
#include "GatewayControlState.h"
#include "ScheduleValidation.h"
#include "ThresholdValidation.h"
#include "config.h"
#include <WiFi.h>
#include <cstring>
#include <math.h>

namespace {
constexpr const char* GH_ATAS_SSID = "Greenhouse-1";
constexpr const char* GH_BAWAH_SSID = "Greenhouse-2";
constexpr const char* GH_DEFAULT_PASSWORD = "change-me-wifi-password";
constexpr unsigned long WIFI_SCAN_RESULT_CACHE_MS = 60000UL;
constexpr unsigned long WIFI_CHANGE_RESULT_CACHE_MS = 180000UL;
constexpr unsigned long DISCONNECT_REASON_MAX_AGE_MS = 45000UL;
constexpr size_t ADMIN_WS_CLIENT_SLOT_COUNT = 8;
constexpr unsigned long ADMIN_WS_CLIENT_TTL_MS = 30UL * 60UL * 1000UL;
constexpr size_t MUTATION_RESULT_CACHE_SIZE = 8;
constexpr unsigned long MUTATION_RESULT_CACHE_TTL_MS = 15000UL;
volatile int g_lastStaDisconnectReason = 0;
volatile unsigned long g_lastStaDisconnectAtMs = 0;
bool g_wifiDisconnectEventRegistered = false;

struct AdminWsClientSlot
{
    uint32_t clientId = 0;
    bool admin = false;
    unsigned long grantedAtMs = 0;
};

AdminWsClientSlot g_adminWsClientSlots[ADMIN_WS_CLIENT_SLOT_COUNT];

struct CachedMutationResult
{
    String payload;
    unsigned long createdAtMs = 0;
};

CachedMutationResult g_cachedMutationResults[MUTATION_RESULT_CACHE_SIZE];
size_t g_cachedMutationResultNext = 0;

String clientBinding(AsyncWebSocketClient* client)
{
    if (!client)
        return "";
    return client->remoteIP().toString();
}

AdminWsClientSlot* findAdminWsClientSlot(AsyncWebSocketClient* client)
{
    if (!client)
        return nullptr;

    const unsigned long now = millis();
    AdminWsClientSlot* emptySlot = nullptr;
    AdminWsClientSlot* expiredSlot = nullptr;

    for (auto& slot : g_adminWsClientSlots)
    {
        if (slot.clientId == client->id())
            return &slot;
        if (slot.clientId == 0 && !emptySlot)
            emptySlot = &slot;
        else if (slot.clientId != 0 &&
                 (!slot.admin || (now - slot.grantedAtMs) > ADMIN_WS_CLIENT_TTL_MS) &&
                 !expiredSlot)
            expiredSlot = &slot;
    }

    if (emptySlot)
        return emptySlot;
    if (expiredSlot)
        return expiredSlot;
    return &g_adminWsClientSlots[0];
}

bool isAdminWsClient(AsyncWebSocketClient* client)
{
    AdminWsClientSlot* slot = findAdminWsClientSlot(client);
    if (!slot || slot->clientId != client->id() || !slot->admin)
        return false;
    if ((millis() - slot->grantedAtMs) > ADMIN_WS_CLIENT_TTL_MS)
    {
        slot->clientId = 0;
        slot->admin = false;
        slot->grantedAtMs = 0;
        return false;
    }
    return true;
}

void setAdminWsClient(AsyncWebSocketClient* client, bool admin)
{
    AdminWsClientSlot* slot = findAdminWsClientSlot(client);
    if (!slot || !client)
        return;
    slot->clientId = client->id();
    slot->admin = admin;
    slot->grantedAtMs = admin ? millis() : 0;
}

void clearAdminWsClient(AsyncWebSocketClient* client)
{
    AdminWsClientSlot* slot = findAdminWsClientSlot(client);
    if (!slot || !client || slot->clientId != client->id())
        return;
    slot->clientId = 0;
    slot->admin = false;
    slot->grantedAtMs = 0;
}

bool encryptWsPayload(const String& plain, String& encrypted)
{
    return CryptoUtils::encryptNodeMediniPayload(plain.c_str(), plain.length(), encrypted);
}

bool sendEncryptedText(AsyncWebSocketClient* client, const String& plain)
{
    if (!client)
        return false;
    String encrypted;
    if (!encryptWsPayload(plain, encrypted))
        return false;
    client->text(encrypted);
    return true;
}

bool broadcastEncryptedText(AsyncWebSocket* ws, const String& plain)
{
    if (!ws)
        return false;
    String encrypted;
    if (!encryptWsPayload(plain, encrypted))
        return false;
    ws->textAll(encrypted);
    return true;
}

bool decodeIncomingWsPayload(const uint8_t* data, size_t len, String& payload, bool& secureFrame)
{
    secureFrame = false;
    payload = "";
    if (!data || len == 0)
        return false;

    if (CryptoUtils::isNodeMediniEncryptedPayload(data, len))
    {
        char decrypted[CryptoUtils::MAX_DECRYPTED_SIZE];
        size_t decryptedLen = 0;
        if (!CryptoUtils::decryptNodeMediniPayload(reinterpret_cast<const char*>(data),
                                                   len,
                                                   decrypted,
                                                   sizeof(decrypted),
                                                   &decryptedLen))
        {
            return false;
        }
        payload = String(decrypted, decryptedLen);
        secureFrame = true;
        return true;
    }

    payload.reserve(len + 1);
    for (size_t i = 0; i < len; ++i)
        payload += static_cast<char>(data[i]);
    return payload.length() > 0;
}

void onWifiStaDisconnected(arduino_event_id_t event, arduino_event_info_t info)
{
    if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        return;
    g_lastStaDisconnectReason = static_cast<int>(info.wifi_sta_disconnected.reason);
    g_lastStaDisconnectAtMs = millis();
}

const char* defaultPasswordForKnownSsid(const char* ssid)
{
    if (!ssid)
        return nullptr;
    if (strcmp(ssid, GH_ATAS_SSID) == 0)
        return GH_DEFAULT_PASSWORD;
    if (strcmp(ssid, GH_BAWAH_SSID) == 0)
        return GH_DEFAULT_PASSWORD;
    return nullptr;
}

const char* wifiStatusToText(wl_status_t status)
{
    switch (status)
    {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "SSID_NOT_FOUND";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    case WL_NO_SHIELD: return "WIFI_OFF";
    default: return "UNKNOWN";
    }
}

const char* disconnectReasonToText(int reason)
{
    if (reason <= 0)
        return "";
    const char* reasonText = WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason));
    if (reasonText && reasonText[0] != '\0')
        return reasonText;
    return "UNKNOWN_REASON";
}

String hintFromFailure(wl_status_t status, int disconnectReason, bool ssidSeen, int rssi, bool isOpenNetwork, bool passwordProvided, int scanCode, bool assumeHidden)
{
    if (!passwordProvided && ssidSeen && !isOpenNetwork)
        return "Password kosong, tapi jaringan ini butuh password.";

    if (!ssidSeen && scanCode < 0)
        return "Scan ulang WiFi di gateway gagal sementara. Coba ulang 1x atau restart gateway/router.";

    if (assumeHidden && !ssidSeen)
        return "SSID target diperlakukan hidden, jadi bisa tidak muncul di scan. Fokus cek password dan keamanan router.";

    // Reason 201: NO_AP_FOUND
    if (status == WL_NO_SSID_AVAIL || disconnectReason == 201 || !ssidSeen)
        return "SSID tidak terlihat oleh gateway. Pastikan router 2.4GHz aktif dan SSID tidak hidden.";

    // Common auth/handshake failures (2, 15, 23, 202, 203, 204)
    if (disconnectReason == 2 || disconnectReason == 15 || disconnectReason == 23 ||
        disconnectReason == 202 || disconnectReason == 203 || disconnectReason == 204)
        return "Autentikasi gagal. Cek password dan mode keamanan router (WPA2/WPA mixed).";

    // Reason 200: BEACON_TIMEOUT, 205: CONNECTION_FAIL
    if (disconnectReason == 200 || disconnectReason == 205)
        return "Koneksi terputus saat proses join AP. Coba dekati gateway ke router dan restart router.";

    // Reason 8: ASSOC_LEAVE (sering muncul saat perangkat sengaja leave AP lama).
    if (disconnectReason == 8)
        return "Perangkat meninggalkan AP lama. Jika tetap gagal, cek band 2.4GHz dan nonaktifkan AP isolation/limit client.";

    if (ssidSeen && rssi <= -82)
        return "Sinyal WiFi lemah (" + String(rssi) + " dBm). Dekatkan gateway ke access point.";

    if (status == WL_CONNECT_FAILED || status == WL_DISCONNECTED)
        return "Gateway gagal menyelesaikan koneksi. Periksa channel 2.4GHz dan kapasitas client router.";

    return "Periksa SSID, password, sinyal, dan pastikan router menyediakan WiFi 2.4GHz.";
}

void appendScheduleArray(JsonArray target, const ScheduleConfig* scheduleSnapshot)
{
    if (!scheduleSnapshot)
        return;
    for (int i = 0; i < MAX_SCHEDULES; i++)
    {
        JsonObject s = target.add<JsonObject>();
        s["id"] = scheduleSnapshot[i].id;
        s["en"] = scheduleSnapshot[i].active;
        s["sh"] = scheduleSnapshot[i].startHour;
        s["sm"] = scheduleSnapshot[i].startMin;
        s["eh"] = scheduleSnapshot[i].endHour;
        s["em"] = scheduleSnapshot[i].endMin;
        char rStr[4];
        snprintf(rStr, sizeof(rStr), "%c%c%c", scheduleSnapshot[i].r1Mode, scheduleSnapshot[i].r2Mode, scheduleSnapshot[i].r3Mode);
        s["relay_code"] = rStr;
        s["r1e"] = (scheduleSnapshot[i].r1Mode != '2');
        s["r1s"] = (scheduleSnapshot[i].r1Mode == '1');
        s["r2e"] = (scheduleSnapshot[i].r2Mode != '2');
        s["r2s"] = (scheduleSnapshot[i].r2Mode == '1');
        s["r3e"] = (scheduleSnapshot[i].r3Mode != '2');
        s["r3s"] = (scheduleSnapshot[i].r3Mode == '1');
    }
}

void appendThresholdProfile(JsonObject target, const ThresholdProfile& profile, const char* source, bool valid)
{
    target["t_min"] = profile.tempMin;
    target["t_max"] = profile.tempMax;
    target["h_min"] = profile.humMin;
    target["h_max"] = profile.humMax;
    target["l_min"] = profile.lightMin;
    target["l_max"] = profile.lightMax;
    target["source"] = source ? source : "UNKNOWN";
    target["valid"] = valid;
}

void cacheMutationResultPayload(const String& payload)
{
    if (payload.isEmpty())
        return;
    g_cachedMutationResults[g_cachedMutationResultNext].payload = payload;
    g_cachedMutationResults[g_cachedMutationResultNext].createdAtMs = millis();
    g_cachedMutationResultNext = (g_cachedMutationResultNext + 1U) % MUTATION_RESULT_CACHE_SIZE;
}

void buildStatusDoc(JsonDocument& doc, SensorDataManager& sensorData, RelayController& relay, MyNetworkManager& net, RTCManager& rtc)
{
    const unsigned long nowMs = millis();
    const bool netConnected = net.isConnected();
    GatewayControlState controlState = {};
    if (!tryGetCachedGatewayControlState(controlState, WEB_UPDATE_INTERVAL_MS))
    {
        controlState = resolveAndCacheGatewayControlState(sensorData,
                                                         relay,
                                                         rtc,
                                                         netConnected,
                                                         nowMs);
    }
    const float temperature = sensorData.temperature;
    const float humidity = sensorData.humidity;
    const float light = sensorData.light;
    const bool fogStatus = sensorData.isFoggy;
    const String mode = sensorData.getModeString();
    const String modeConfig = sensorData.getConfiguredModeString();
    const char* runtimeSource = controlState.runtimeDataSource;
    const bool thresholdEditable = controlState.thresholdEditable;
    const bool scheduleEditable = controlState.scheduleEditable;
    const ThresholdProfile& runtimeThresholds = sensorData.getRuntimeThresholdProfile();
    const ThresholdProfile& edgeThresholds = sensorData.getLocalThresholdProfile();
    const ThresholdProfile& cloudThresholds = sensorData.getCloudThresholdProfile();
    const char* runtimeThresholdSource = controlState.thresholdRuntimeSource;
    const char* configuredThresholdSource = controlState.thresholdConfiguredSource;
    const bool wifiConnected = net.isWiFiConnected();
    const int signal = net.getSignalQuality();
    const String ipAddress = wifiConnected ? WiFi.localIP().toString() : "N/A";
    const String activeSsid = net.getActiveSSID();
    const DataSourceMode currentMode = controlState.configuredMode;
    const bool usingLocalData = controlState.runtimeUsesLocalData;
    const bool timeValid = rtc.isTimeSet();
    NodeDataEntry nodeSnapshot[MAX_NODES];
    memcpy(nodeSnapshot, sensorData.getAllNodes(), sizeof(nodeSnapshot));
    const bool useLocalSchedules = usingLocalData;
    const bool edgeScheduleAvailable = controlState.edgeScheduleAvailable;
    const bool cloudScheduleAvailable = controlState.cloudScheduleAvailable;
    const bool scheduleConfigured = controlState.runtimeScheduleConfigured;
    const bool scheduleRuntimeReady = controlState.runtimeScheduleReady;
    ScheduleConfig runtimeScheduleSnapshot[MAX_SCHEDULES];
    ScheduleConfig edgeScheduleSnapshot[MAX_SCHEDULES];
    ScheduleConfig cloudScheduleSnapshot[MAX_SCHEDULES];
    memcpy(runtimeScheduleSnapshot, relay.getSchedulesForMode(useLocalSchedules), sizeof(runtimeScheduleSnapshot));
    memcpy(edgeScheduleSnapshot, relay.getSchedulesForMode(true), sizeof(edgeScheduleSnapshot));
    memcpy(cloudScheduleSnapshot, relay.getSchedulesForMode(false), sizeof(cloudScheduleSnapshot));
    const bool relayExhaust = relay.getR1();
    const bool relayDehumidifier = relay.getR2();
    const bool relayBlower = relay.getR3();

    doc["gh_id"] = GH_ID_CONFIG;
    doc["firmware"] = FIRMWARE_VERSION;
    doc["epoch"] = time(nullptr);

    JsonObject sensors = doc["sensors"].to<JsonObject>();
    sensors["temperature"] = temperature;
    sensors["humidity"] = humidity;
    sensors["light"] = light;
    sensors["mode"] = mode;
    sensors["mode_config"] = modeConfig;
    sensors["runtime_source"] = runtimeSource;
    sensors["threshold_editable"] = thresholdEditable;
    sensors["schedule_editable"] = scheduleEditable;
    sensors["threshold_runtime_source"] = runtimeThresholdSource;
    sensors["threshold_config_source"] = configuredThresholdSource;
#if GH_ID_CONFIG == 2
    sensors["fog_status"] = fogStatus;
#endif

    JsonObject th = doc["thresholds"].to<JsonObject>();
    appendThresholdProfile(th, runtimeThresholds, runtimeThresholdSource, true);
    appendThresholdProfile(doc["thresholds_runtime"].to<JsonObject>(), runtimeThresholds, runtimeThresholdSource, true);
    appendThresholdProfile(doc["thresholds_edge"].to<JsonObject>(), edgeThresholds, "EDGE", true);
    appendThresholdProfile(doc["thresholds_cloud"].to<JsonObject>(), cloudThresholds, "CLOUD", sensorData.hasCloudThresholdProfile());

    JsonObject relays = doc["relays"].to<JsonObject>();
    relays["exhaust"] = relayExhaust;
    relays["dehumidifier"] = relayDehumidifier;
    relays["blower"] = relayBlower;

    JsonObject network = doc["network"].to<JsonObject>();
    network["connected"] = netConnected;
    network["type"] = !netConnected ? "OFFLINE" : (wifiConnected ? "WiFi" : "GPRS");
    network["signal"] = signal;
    network["ip"] = ipAddress;
    network["ssid"] = activeSsid;
    const bool cloudSyncActive = strcmp(controlState.cloudSyncMode, "off") != 0;
    const bool cloudRecoveryMode = strcmp(controlState.cloudSyncMode, "recovery") == 0;
    network["cloud_polling"] = cloudSyncActive;
    network["cloud_sync_mode"] = !cloudSyncActive ? "off" : (cloudRecoveryMode ? "recovery" : "active");

    JsonObject control = doc["control"].to<JsonObject>();
    control["time_valid"] = timeValid;
    control["schedule_required"] = scheduleConfigured;
    control["schedule_ready"] = scheduleRuntimeReady;
    control["schedule_degraded"] = !scheduleRuntimeReady;
    control["schedule_source"] = controlState.scheduleEffectiveSource;
    control["schedule_runtime_source"] = controlState.scheduleRuntimeSource;
    control["schedule_edge_available"] = edgeScheduleAvailable;
    control["schedule_cloud_available"] = cloudScheduleAvailable;

    JsonArray nodeArr = doc["nodes"].to<JsonArray>();
    const unsigned long now = nowMs;

    for (int i = 0; i < MAX_NODES; i++)
    {
        if (!nodeSnapshot[i].active)
            continue;
        JsonObject n = nodeArr.add<JsonObject>();
        n["name"] = nodeSnapshot[i].node_name;
        n["t"] = nodeSnapshot[i].temp;
        n["h"] = nodeSnapshot[i].hum;
        n["l"] = nodeSnapshot[i].lux;
        n["is_f"] = nodeSnapshot[i].is_foggy;
        n["conf"] = nodeSnapshot[i].confidence;
        n["age"] = (now - nodeSnapshot[i].lastMillis) / 1000;
    }

    appendScheduleArray(doc["schedules"].to<JsonArray>(), runtimeScheduleSnapshot);
    appendScheduleArray(doc["schedules_runtime"].to<JsonArray>(), runtimeScheduleSnapshot);
    appendScheduleArray(doc["schedules_edge"].to<JsonArray>(), edgeScheduleSnapshot);
    appendScheduleArray(doc["schedules_cloud"].to<JsonArray>(), cloudScheduleSnapshot);
}
} // namespace

AsyncWebSocket *WebSocketManager::_ws = nullptr;
WebSocketManager *WebSocketManager::_instance = nullptr;

WebSocketManager::WebSocketManager(SensorDataManager &data, RelayController &relays, MyNetworkManager &net, RTCManager& rtc)
    : sensorData_ref(data), relay_ref(relays), net_ref(net), rtc_ref(rtc) { _instance = this; }

void WebSocketManager::begin(AsyncWebServer *server)
{
    if (!g_wifiDisconnectEventRegistered)
    {
        WiFi.onEvent(onWifiStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        g_wifiDisconnectEventRegistered = true;
    }

    _ws = new AsyncWebSocket("/status_ws");
    _ws->onEvent(onWsEvent);
    server->addHandler(_ws);
}

void WebSocketManager::broadcastStatus()
{
    if (!_ws)
        return;

    JsonDocument doc;
    buildStatusDoc(doc, sensorData_ref, relay_ref, net_ref, rtc_ref);
    cachedStatusPayload = "";
    serializeJson(doc, cachedStatusPayload);
    broadcastEncryptedText(_ws, cachedStatusPayload);
}

void WebSocketManager::sendMutationResultToClient(uint32_t clientId, const char* type, bool success, const String& message, int idx, const char* requestId)
{
    if (!_ws || !type)
        return;

    JsonDocument response;
    response["type"] = type;
    response["success"] = success;
    response["message"] = message;
    if (idx >= 0)
        response["idx"] = idx;
    if (requestId && requestId[0] != '\0')
        response["request_id"] = requestId;

    String payload;
    serializeJson(response, payload);
    cacheMutationResultPayload(payload);

    if (clientId == 0)
        return;

    AsyncWebSocketClient* client = _ws->client(clientId);
    if (!client)
        return;
    sendEncryptedText(client, payload);
}

void WebSocketManager::sendRecentMutationResults(AsyncWebSocketClient* client)
{
    if (!client)
        return;

    const unsigned long now = millis();
    for (size_t i = 0; i < MUTATION_RESULT_CACHE_SIZE; ++i)
    {
        CachedMutationResult& item = g_cachedMutationResults[i];
        if (item.payload.isEmpty() || item.createdAtMs == 0)
            continue;
        if ((now - item.createdAtMs) > MUTATION_RESULT_CACHE_TTL_MS)
            continue;
        sendEncryptedText(client, item.payload);
    }
}

void WebSocketManager::pumpAsyncEvents()
{
    if (wifiScanAwaitingResult)
    {
        if (wifiScanUsesStandaloneScan)
        {
            const int scanResult = WiFi.scanComplete();
            if (scanResult != WIFI_SCAN_RUNNING)
            {
                if (scanResult >= 0)
                    net_ref.getCredentialStore().updateFromScan(scanResult);
                else
                    net_ref.getCredentialStore().noteScanFailure(scanResult);
                WiFi.scanDelete();
                sendWifiScanResult(nullptr);
                wifiScanAwaitingResult = false;
                wifiScanUsesStandaloneScan = false;
                pendingWifiScanRequestedAtMs = 0;
            }
        }
        else
        {
            const unsigned long scanCompletedAtMs = net_ref.getLastWiFiScanCompletedMs();
            if (scanCompletedAtMs != 0 && scanCompletedAtMs >= pendingWifiScanRequestedAtMs)
            {
                sendWifiScanResult(nullptr);
                wifiScanAwaitingResult = false;
                pendingWifiScanRequestedAtMs = 0;
            }
        }
    }

    if (!wifiChangeAwaitingResult)
        return;

    SpecificWiFiConnectResult result;
    if (!net_ref.consumeSpecificWiFiConnectResult(result))
        return;

    JsonDocument response;
    const String targetSsid = pendingWifiTargetSsid.length() > 0 ? pendingWifiTargetSsid : result.requestedSsid;
    const bool forceHidden = result.forceHidden || pendingWifiForceHidden;
    const wl_status_t status = static_cast<wl_status_t>(result.wifiStatus);
    int disconnectReason = 0;
    if (g_lastStaDisconnectAtMs > 0 && (millis() - g_lastStaDisconnectAtMs) <= DISCONNECT_REASON_MAX_AGE_MS)
        disconnectReason = g_lastStaDisconnectReason;
    const char* disconnectReasonText = disconnectReasonToText(disconnectReason);

    response["type"] = "wifi_change_result";
    response["epoch"] = time(nullptr);
    response["success"] = result.success;
    response["ssid"] = targetSsid;
    response["connected_ssid"] = result.connectedSsid;
    response["wifi_status"] = result.wifiStatus;
    response["wifi_status_text"] = wifiStatusToText(status);
    response["auto_password"] = pendingWifiAutoPasswordApplied;
    response["force_hidden"] = forceHidden;
    response["disconnect_reason"] = disconnectReason;
    response["disconnect_reason_text"] = disconnectReasonText;

    if (result.success)
    {
        response["message"] = "WiFi berhasil diganti";
        if (net_ref.isWiFiConnected())
            response["ip"] = WiFi.localIP().toString();
        broadcastStatus();
    }
    else
    {
        String hint = hintFromFailure(
            status,
            disconnectReason,
            result.scanSeen,
            result.scanRssi,
            result.scanOpenNetwork,
            pendingWifiPasswordProvided,
            result.scanCode,
            forceHidden);

        response["message"] = "Gagal ganti WiFi";
        response["hint"] = hint;
        response["scan_seen"] = result.scanSeen;
        response["scan_code"] = result.scanCode;
        if (result.scanSeen)
        {
            response["scan_rssi"] = result.scanRssi;
            response["scan_open_network"] = result.scanOpenNetwork;
        }

        String detail = String("Gagal konek ke '") + targetSsid + "'";
        if (result.connectedSsid.length() > 0)
            detail += ", saat ini tersambung ke '" + result.connectedSsid + "'";
        detail += ". status=" + String(result.wifiStatus) + " (" + wifiStatusToText(status) + ")";
        if (disconnectReason > 0)
            detail += ", reason=" + String(disconnectReason) + " (" + disconnectReasonText + ")";
        if (result.scanSeen)
            detail += ", RSSI=" + String(result.scanRssi) + " dBm";
        else if (forceHidden)
            detail += ", SSID tidak terlihat saat scan (mode hidden aktif)";
        else
            detail += ", SSID tidak terlihat saat scan";
        detail += ", scan_code=" + String(result.scanCode);
        response["detail"] = detail;
    }

    cachedWifiChangePayload = "";
    serializeJson(response, cachedWifiChangePayload);
    cachedWifiChangeAtMs = millis();
    if (_ws)
        broadcastEncryptedText(_ws, cachedWifiChangePayload);

    wifiChangeAwaitingResult = false;
    pendingWifiAutoPasswordApplied = false;
    pendingWifiPasswordProvided = false;
    pendingWifiForceHidden = false;
    pendingWifiTargetSsid = "";
}

void WebSocketManager::cleanupClients() { _ws->cleanupClients(); }

void WebSocketManager::sendJsonToClient(AsyncWebSocketClient* client, JsonDocument& doc)
{
    if (!client)
        return;

    String payload;
    serializeJson(doc, payload);
    sendEncryptedText(client, payload);
}

void WebSocketManager::sendCachedStatus(AsyncWebSocketClient* client)
{
    if (!client)
        return;

    if (cachedStatusPayload.isEmpty())
    {
        JsonDocument doc;
        buildStatusDoc(doc, sensorData_ref, relay_ref, net_ref, rtc_ref);
        cachedStatusPayload = "";
        serializeJson(doc, cachedStatusPayload);
    }
    sendEncryptedText(client, cachedStatusPayload);
}

void WebSocketManager::handleAdminAuth(AsyncWebSocketClient* client, const char* password)
{
    JsonDocument response;
    response["type"] = "admin_auth_result";
    response["epoch"] = time(nullptr);
    response["success"] = false;

    const String attempt = password ? String(password) : String();
    unsigned long retryAfterMs = 0;
    if (!verifyAdminPasswordWithRateLimit(clientBinding(client), attempt, &retryAfterMs))
    {
        if (retryAfterMs > 0)
        {
            response["message"] = "Terlalu banyak percobaan login. Coba lagi sebentar.";
            response["retry_after_ms"] = retryAfterMs;
        }
        else
        {
            response["message"] = "Password admin salah";
        }
        sendJsonToClient(client, response);
        return;
    }

    setAdminWsClient(client, true);
    response["success"] = true;
    response["message"] = "Admin auth OK";
    response["admin_token"] = issueAdminSessionToken(clientBinding(client));
    response["expires_in_sec"] = 1800;
    sendJsonToClient(client, response);
}

bool WebSocketManager::isAdminRequestAuthorized(AsyncWebSocketClient* client, JsonDocument& doc)
{
    const char* token = doc["admin_token"] | "";
    if (!isAdminSessionTokenValid(String(token), clientBinding(client)))
        return false;
    if (!isAdminWsClient(client))
        setAdminWsClient(client, true);
    return true;
}

void WebSocketManager::sendAdminAuthRequired(AsyncWebSocketClient* client, const char* action)
{
    JsonDocument response;
    response["type"] = "error";
    response["auth_required"] = true;
    response["message"] = (action && strcmp(action, "secure_transport") == 0)
                              ? "Encrypted WebSocket frame required"
                              : "Admin auth required";
    if (action && action[0] != '\0')
        response["action"] = action;
    sendJsonToClient(client, response);
}

void WebSocketManager::sendCachedWifiScanResult(AsyncWebSocketClient* client)
{
    if (!client || cachedWifiScanPayload.length() == 0 || cachedWifiScanAtMs == 0)
        return;
    if (millis() - cachedWifiScanAtMs > WIFI_SCAN_RESULT_CACHE_MS)
        return;
    sendEncryptedText(client, cachedWifiScanPayload);
}

void WebSocketManager::sendWifiLog(AsyncWebSocketClient* client, const char* level, const String& message)
{
    JsonDocument log;
    log["type"] = "wifi_log";
    log["level"] = level ? level : "info";
    log["epoch"] = time(nullptr);
    log["message"] = message;
    sendJsonToClient(client, log);
}

void WebSocketManager::handleWifiScanRequest(AsyncWebSocketClient* client)
{
    JsonDocument response;
    response["type"] = "wifi_scan_result";
    response["epoch"] = time(nullptr);

    if (wifiScanAwaitingResult)
    {
        response["pending"] = true;
        response["success"] = true;
        response["message"] = "Scan WiFi masih berjalan";
        sendJsonToClient(client, response);
        return;
    }

    response["pending"] = true;
    response["success"] = true;
    response["message"] = "Memulai scan WiFi...";
    sendJsonToClient(client, response);
    sendWifiLog(client, "info", "Memulai scan WiFi async dari dashboard.");

    wifiScanAwaitingResult = true;
    wifiScanUsesStandaloneScan = false;
    pendingWifiScanRequestedAtMs = millis();
    if (net_ref.isWiFiConnected() && !net_ref.isSpecificWiFiConnectPending())
    {
        WiFi.scanDelete();
        const int scanStart = WiFi.scanNetworks(true, true);
        if (scanStart == WIFI_SCAN_FAILED)
        {
            net_ref.getCredentialStore().noteScanFailure(WIFI_SCAN_FAILED);
            sendWifiScanResult(client);
            wifiScanAwaitingResult = false;
        }
        else
        {
            wifiScanUsesStandaloneScan = true;
        }
        return;
    }

    net_ref.requestUiWiFiScan();
}

void WebSocketManager::sendCachedWifiChangeResult(AsyncWebSocketClient* client)
{
    if (!client || cachedWifiChangePayload.length() == 0 || cachedWifiChangeAtMs == 0)
        return;
    if (millis() - cachedWifiChangeAtMs > WIFI_CHANGE_RESULT_CACHE_MS)
        return;
    sendEncryptedText(client, cachedWifiChangePayload);
    cachedWifiChangePayload = "";
    cachedWifiChangeAtMs = 0;
}

void WebSocketManager::sendWifiScanResult(AsyncWebSocketClient* client)
{
    JsonDocument response;
    response["type"] = "wifi_scan_result";
    response["epoch"] = time(nullptr);
    response["pending"] = false;
    response["scan_code"] = net_ref.getLastWiFiScanCode();
    JsonArray networks = response["networks"].to<JsonArray>();
    const WiFiScanRecord* scanResults = net_ref.getLastWiFiScanResults();
    const size_t resultCount = net_ref.getLastWiFiScanResultCount();
    for (size_t i = 0; i < resultCount; ++i)
    {
        if (!scanResults[i].valid || scanResults[i].ssid[0] == '\0')
            continue;

        JsonObject n = networks.add<JsonObject>();
        n["ssid"] = scanResults[i].ssid;
        n["rssi"] = scanResults[i].rssi;
        n["auth"] = scanResults[i].secure;
    }

    response["count"] = networks.size();
    response["success"] = net_ref.getLastWiFiScanCode() >= 0;
    if (net_ref.getLastWiFiScanCode() >= 0)
        response["message"] = "Scan WiFi selesai";
    else
        response["message"] = "Scan WiFi gagal";

    cachedWifiScanPayload = "";
    serializeJson(response, cachedWifiScanPayload);
    cachedWifiScanAtMs = millis();
    if (client)
        sendEncryptedText(client, cachedWifiScanPayload);
    else if (_ws)
        broadcastEncryptedText(_ws, cachedWifiScanPayload);
}

void WebSocketManager::handleWifiChange(AsyncWebSocketClient* client, const char* ssid, const char* password)
{
    JsonDocument response;
    response["type"] = "wifi_change_result";
    response["epoch"] = time(nullptr);

    if (!ssid || strlen(ssid) == 0)
    {
        response["success"] = false;
        response["message"] = "SSID kosong";
        response["hint"] = "Isi SSID terlebih dulu sebelum menekan tombol connect.";
        sendWifiLog(client, "error", "Ganti WiFi dibatalkan: SSID kosong.");
        cachedWifiChangePayload = "";
        serializeJson(response, cachedWifiChangePayload);
        cachedWifiChangeAtMs = millis();
        sendJsonToClient(client, response);
        return;
    }

    String targetSsid = ssid;
    targetSsid.trim();
    String effectivePassword = password ? password : "";
    bool autoPasswordApplied = false;
    const bool forceHiddenTarget = (targetSsid == GH_BAWAH_SSID);

    const char* knownDefault = defaultPasswordForKnownSsid(targetSsid.c_str());
    if (effectivePassword.length() == 0 && knownDefault)
    {
        effectivePassword = knownDefault;
        autoPasswordApplied = true;
    }

    sendWifiLog(client, "info", String("Memulai proses ganti WiFi ke '") + targetSsid + "'.");
    if (autoPasswordApplied)
        sendWifiLog(client, "info", "SSID dikenali, password default gateway diterapkan otomatis.");
    else if (effectivePassword.length() == 0)
        sendWifiLog(client, "warn", "Password kosong. Pastikan SSID memang open network.");
    if (forceHiddenTarget)
        sendWifiLog(client, "warn", "Mode hidden aktif: Greenhouse-2 dipaksa konek sebagai hidden SSID.");

    if (wifiChangeAwaitingResult || net_ref.isSpecificWiFiConnectPending())
    {
        response["success"] = false;
        response["message"] = "Masih ada proses ganti WiFi yang berjalan";
        const String activeTarget = pendingWifiTargetSsid.length() > 0 ? pendingWifiTargetSsid : net_ref.getPendingSpecificWiFiSsid();
        if (activeTarget.length() > 0)
            response["detail"] = String("Target yang masih diproses: '") + activeTarget + "'";
        sendWifiLog(client, "warn", "Request ganti WiFi ditolak karena proses sebelumnya belum selesai.");
        cachedWifiChangePayload = "";
        serializeJson(response, cachedWifiChangePayload);
        cachedWifiChangeAtMs = millis();
        sendJsonToClient(client, response);
        return;
    }

    JsonDocument pending;
    pending["type"] = "wifi_change_result";
    pending["pending"] = true;
    pending["success"] = true;
    pending["ssid"] = targetSsid;
    pending["auto_password"] = autoPasswordApplied;
    pending["force_hidden"] = forceHiddenTarget;
    pending["message"] = "Mencoba pindah WiFi...";
    sendJsonToClient(client, pending);
    sendWifiLog(client, "warn", "Koneksi dashboard bisa terputus sementara saat perangkat pindah SSID/IP.");
    g_lastStaDisconnectReason = 0;
    g_lastStaDisconnectAtMs = 0;
    if (!net_ref.requestSpecificWiFiConnect(targetSsid.c_str(), effectivePassword.c_str(), true, forceHiddenTarget))
    {
        response["success"] = false;
        response["ssid"] = targetSsid;
        response["message"] = "Gateway menolak request ganti WiFi";
        response["hint"] = "Tunggu proses sebelumnya selesai, lalu coba lagi.";
        sendWifiLog(client, "error", "Gateway menolak request ganti WiFi.");
        cachedWifiChangePayload = "";
        serializeJson(response, cachedWifiChangePayload);
        cachedWifiChangeAtMs = millis();
        sendJsonToClient(client, response);
        return;
    }

    wifiChangeAwaitingResult = true;
    pendingWifiAutoPasswordApplied = autoPasswordApplied;
    pendingWifiPasswordProvided = effectivePassword.length() > 0;
    pendingWifiForceHidden = forceHiddenTarget;
    pendingWifiTargetSsid = targetSsid;
}

void WebSocketManager::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    if (type == WS_EVT_CONNECT && _instance)
    {
        _instance->sendCachedStatus(client);
        _instance->sendRecentMutationResults(client);
        _instance->sendCachedWifiScanResult(client);
        _instance->sendCachedWifiChangeResult(client);
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        clearAdminWsClient(client);
    }
    else if (type == WS_EVT_DATA)
    {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)
        {
            String payload;
            bool secureFrame = false;
            if (!decodeIncomingWsPayload(data, len, payload, secureFrame))
                return;
            if (!secureFrame)
            {
                if (_instance)
                    _instance->sendAdminAuthRequired(client, "secure_transport");
                return;
            }

            JsonDocument doc;
            if (!deserializeJson(doc, payload) && _instance)
            {
                const char *requestId = doc["request_id"] | "";
                const auto sendMutationResult = [&](const char* type, bool success, const String& message, int idx = -1) {
                    JsonDocument response;
                    response["type"] = type;
                    response["success"] = success;
                    response["message"] = message;
                    if (idx >= 0)
                        response["idx"] = idx;
                    if (requestId && requestId[0] != '\0')
                        response["request_id"] = requestId;
                    _instance->sendJsonToClient(client, response);
                };
                const char *msgType = doc["type"];
                if (msgType && strcmp(msgType, "set_thresholds") == 0)
                {
                    const float tMin = doc["tm_min"] | NAN;
                    const float tMax = doc["tm_max"] | NAN;
                    const float hMin = doc["hm_min"] | NAN;
                    const float hMax = doc["hm_max"] | NAN;

                    if (!_instance->sensorData_ref.canEditLocalThresholds())
                    {
                        sendMutationResult("set_thresholds_result", false, "Thresholds read-only pada mode saat ini.");
                    }
                    else if (!isfinite(tMin) || !isfinite(tMax) || !isfinite(hMin) || !isfinite(hMax))
                    {
                        sendMutationResult("set_thresholds_result", false, "Payload threshold tidak valid.");
                    }
                    else if (!ThresholdValidation::isTemperatureRangeValid(tMin, tMax))
                    {
                        sendMutationResult("set_thresholds_result", false, "Threshold suhu harus di rentang -20..80 dengan min < max.");
                    }
                    else if (!ThresholdValidation::isHumidityRangeValid(hMin, hMax))
                    {
                        sendMutationResult("set_thresholds_result", false, "Threshold humidity harus di rentang 0..100 dengan min < max.");
                    }
                    else if (!enqueueLocalThresholdMutation(tMin, tMax, hMin, hMax, client ? client->id() : 0, requestId))
                    {
                        sendMutationResult("set_thresholds_result", false, "State update queue penuh.");
                    }
                }
                else if (msgType && strcmp(msgType, "set_schedule") == 0)
                {
                    const int idx = doc["idx"] | -1;
                    const int sh = doc["sh"] | -1;
                    const int sm = doc["sm"] | -1;
                    const int eh = doc["eh"] | -1;
                    const int em = doc["em"] | -1;

                    if (!_instance->sensorData_ref.canEditLocalSchedules())
                    {
                        sendMutationResult("set_schedule_result", false, "Schedule read-only pada mode saat ini.", idx);
                    }
                    else if (idx < 0 || idx >= MAX_SCHEDULES)
                    {
                        sendMutationResult("set_schedule_result", false, "Index schedule tidak valid.", idx);
                    }
                    else if (!ScheduleValidation::isTimeComponentValid(sh, sm) ||
                             !ScheduleValidation::isTimeComponentValid(eh, em))
                    {
                        sendMutationResult("set_schedule_result", false, "Waktu schedule tidak valid.", idx);
                    }
                    else
                    {
                        ScheduleConfig cfg;
                        cfg.id = doc["id"] | 0;
                        cfg.active = doc["en"];
                        cfg.startHour = static_cast<uint8_t>(sh);
                        cfg.startMin = static_cast<uint8_t>(sm);
                        cfg.endHour = static_cast<uint8_t>(eh);
                        cfg.endMin = static_cast<uint8_t>(em);
                        auto getMode = [&](bool en, bool state) -> char
                        { return !en ? '2' : (state ? '1' : '0'); };
                        cfg.r1Mode = getMode(doc["r1e"], doc["r1s"]);
                        cfg.r2Mode = getMode(doc["r2e"], doc["r2s"]);
                        cfg.r3Mode = getMode(doc["r3e"], doc["r3s"]);
                        if (!ScheduleValidation::isScheduleConfigValid(cfg))
                        {
                            sendMutationResult("set_schedule_result", false, "Schedule aktif harus punya waktu valid dan durasi non-zero.", idx);
                        }
                        else if (!enqueueLocalScheduleMutation(idx, cfg, client ? client->id() : 0, requestId))
                        {
                            sendMutationResult("set_schedule_result", false, "State update queue penuh.", idx);
                        }
                    }
                }
                else if (msgType && strcmp(msgType, "wifi_scan") == 0)
                {
                    _instance->handleWifiScanRequest(client);
                }
                else if (msgType && strcmp(msgType, "admin_auth") == 0)
                {
                    const char* password = doc["password"] | "";
                    _instance->handleAdminAuth(client, password);
                }
                else if (msgType && strcmp(msgType, "wifi_change") == 0)
                {
                    if (!_instance->isAdminRequestAuthorized(client, doc))
                    {
                        _instance->sendAdminAuthRequired(client, "wifi_change");
                        return;
                    }
                    const char* ssid = doc["ssid"] | "";
                    const char* pass = doc["pass"] | "";
                    _instance->handleWifiChange(client, ssid, pass);
                }
                else if (msgType && strcmp(msgType, "client_time") == 0)
                {
                    const bool allowUnauthedBootstrap = !_instance->rtc_ref.isTimeSet();
                    if (!allowUnauthedBootstrap && !_instance->isAdminRequestAuthorized(client, doc))
                    {
                        _instance->sendAdminAuthRequired(client, "client_time");
                        return;
                    }
                    uint32_t epochSec = 0;
                    if (doc["epoch"].is<unsigned long>())
                    {
                        epochSec = doc["epoch"].as<unsigned long>();
                    }
                    else if (doc["epoch"].is<long>())
                    {
                        const long v = doc["epoch"].as<long>();
                        if (v > 0)
                            epochSec = static_cast<uint32_t>(v);
                    }

                    if (epochSec > 1672531200UL)
                        enqueueClientEpochMutation(epochSec);
                }
            }
        }
    }
}
