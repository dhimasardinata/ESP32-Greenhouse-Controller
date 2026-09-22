///////////////////////////////////////////////////////////////////////////////////
// File: MyNetworkManager.cpp (STABLE VERSION)
//
// Updates:
// - Removed Heap Allocation (prevent crashes)
// - Added HTTP 1.0 mode (fix SSL EOF errors)
// - Changed to String buffering (fix JSON Deserialization errors)
///////////////////////////////////////////////////////////////////////////////////

#include "MyNetworkManager.h"
#include "ConfigManager.h"
#include <WiFi.h>
#include "config.h"
#include <esp_task_wdt.h>
#include <TinyGsmCommon.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <math.h>
#include "SensorNormalization.h"
#include "ScheduleValidation.h"
#include "ThresholdValidation.h"

#ifndef RELAY_API_ROOT
#define RELAY_API_ROOT "https://your-relay-server.example.com/api"
#endif

#define SerialAT Serial1

namespace {
constexpr size_t kGatewayUserAgentLen = 40;
constexpr size_t kGatewayDeviceIdLen = 16;
constexpr char kAtomicOriginHttps[] = "https://your-server.example.com";
constexpr char kAtomicOriginHttp[] = "http://your-server.example.com";
constexpr char kTaOriginHttps[] = "https://your-ta-server.example.com";
constexpr char kTaOriginHttp[] = "http://your-ta-server.example.com";
constexpr char kRelayAtomicPrefix[] = RELAY_API_ROOT "/atomic";
constexpr char kRelayTaPrefix[] = RELAY_API_ROOT "/ta";
constexpr uint16_t kGprsProbeAtTimeoutMs = 400U;
constexpr uint16_t kGprsShortCommandTimeoutMs = 700U;
constexpr uint16_t kGprsMediumCommandTimeoutMs = 1200U;
constexpr uint16_t kGprsLongCommandTimeoutMs = 2000U;
constexpr uint16_t kThresholdFetchTimeoutMs = HTTP_REQUEST_TIMEOUT_MS + 4000U;
constexpr unsigned long kGprsAtProbeWindowMs = 4000UL;
constexpr unsigned long kGprsSetupAttemptWindowMs = 45000UL;
constexpr unsigned long kGprsHealthPollIntervalMs = 10000UL;
constexpr unsigned long kGprsSignalPollIntervalMs = 15000UL;
constexpr unsigned long kGprsVerifiedMaxAgeMs = 30000UL;

void copyText(char* out, size_t out_len, const char* text)
{
    if (!out || out_len == 0)
        return;

    if (!text)
        text = "";

    strncpy(out, text, out_len - 1);
    out[out_len - 1] = '\0';
}

void buildGatewayUserAgent(char* out, size_t out_len)
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

void buildGatewayDeviceId(char* out, size_t out_len)
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

const char* wlStatusName(wl_status_t status)
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

const char* wifiStateName(MyNetworkManager::WiFiState state)
{
    switch (state)
    {
    case MyNetworkManager::WiFiState::IDLE: return "IDLE";
    case MyNetworkManager::WiFiState::SCANNING: return "SCANNING";
    case MyNetworkManager::WiFiState::CONNECTING: return "CONNECTING";
    case MyNetworkManager::WiFiState::CONNECTED: return "CONNECTED";
    case MyNetworkManager::WiFiState::RETRY_DELAY: return "RETRY_DELAY";
    case MyNetworkManager::WiFiState::FALLBACK_GPRS_WAIT: return "FALLBACK_GPRS_WAIT";
    case MyNetworkManager::WiFiState::FALLBACK_GPRS_MODEM_INIT: return "FALLBACK_GPRS_MODEM_INIT";
    case MyNetworkManager::WiFiState::FALLBACK_GPRS_SIM_READY: return "FALLBACK_GPRS_SIM_READY";
    case MyNetworkManager::WiFiState::FALLBACK_GPRS_NETWORK_ATTACH: return "FALLBACK_GPRS_NETWORK_ATTACH";
    case MyNetworkManager::WiFiState::FALLBACK_GPRS_CONNECTING: return "FALLBACK_GPRS_CONNECTING";
    default: return "UNKNOWN";
    }
}

const char* uplinkModeName(MyNetworkManager::UplinkMode mode)
{
    switch (mode)
    {
    case MyNetworkManager::UplinkMode::DIRECT: return "DIRECT";
    case MyNetworkManager::UplinkMode::RELAY: return "RELAY";
    case MyNetworkManager::UplinkMode::AUTO:
    default:
        return "AUTO";
    }
}

String describeHttpFailure(int httpStatusCode, bool wafBlocked)
{
    if (wafBlocked)
        return String("Imunify360 blocked");

    if (httpStatusCode < 0)
    {
        const String err = HTTPClient::errorToString(httpStatusCode);
        return err.length() > 0 ? err : String("connection/network error");
    }

    return String("HTTP ") + String(httpStatusCode);
}

uint32_t timeoutMsToClientSeconds(uint32_t timeoutMs)
{
    uint32_t seconds = timeoutMs / 1000U;
    if ((timeoutMs % 1000U) != 0U)
        ++seconds;
    return seconds == 0U ? 1U : seconds;
}

bool isSensitiveQueryKey(const String& key)
{
    String lower = key;
    lower.toLowerCase();
    return lower == "token" ||
           lower == "auth" ||
           lower == "authorization" ||
           lower == "auth_token" ||
           lower == "api_key" ||
           lower == "apikey" ||
           lower == "key" ||
           lower.endsWith("_token") ||
           lower.endsWith("_key");
}

String sanitizeUrlForLog(const String& url)
{
    const int queryPos = url.indexOf('?');
    if (queryPos < 0)
        return url;

    const int fragmentPos = url.indexOf('#', queryPos);
    const String base = url.substring(0, queryPos + 1);
    const String query = fragmentPos >= 0 ? url.substring(queryPos + 1, fragmentPos)
                                          : url.substring(queryPos + 1);
    const String fragment = fragmentPos >= 0 ? url.substring(fragmentPos) : String();

    String sanitized = base;
    int start = 0;
    bool first = true;
    while (start <= query.length()) {
        const int ampPos = query.indexOf('&', start);
        const int endPos = ampPos >= 0 ? ampPos : query.length();
        String part = query.substring(start, endPos);

        if (!part.isEmpty()) {
            const int eqPos = part.indexOf('=');
            const String key = eqPos >= 0 ? part.substring(0, eqPos) : part;
            if (isSensitiveQueryKey(key)) {
                part = key + "=REDACTED";
            }
            if (!first)
                sanitized += '&';
            sanitized += part;
            first = false;
        }

        if (ampPos < 0)
            break;
        start = ampPos + 1;
    }

    sanitized += fragment;
    return sanitized;
}

void logTlsClientLastError(WiFiClientSecure& client, const char* phase, const String& url)
{
    char tlsError[128];
    tlsError[0] = '\0';
    const int tlsCode = client.lastError(tlsError, sizeof(tlsError));
    if (tlsCode == 0)
        return;

    const String loggedUrl = sanitizeUrlForLog(url);
    Serial.printf("[TLS] %s err=%d (%s) | URL: %s\n",
                  phase ? phase : "request",
                  tlsCode,
                  tlsError[0] != '\0' ? tlsError : "unknown",
                  loggedUrl.c_str());
}

bool responseBodyIndicatesWafBlock(const char* body)
{
    if (!body || body[0] == '\0')
        return false;

    return strstr(body, "Access denied by Imunify360") != nullptr ||
           strstr(body, "bot-protection") != nullptr ||
           strstr(body, "automation should be whitelisted") != nullptr;
}

bool parseCloudFloat(JsonVariantConst value, float& outValue)
{
    if (value.isNull())
        return false;

    if (value.is<const char*>())
    {
        const char* raw = value.as<const char*>();
        if (!raw || raw[0] == '\0')
            return false;

        char* endPtr = nullptr;
        const float parsed = strtof(raw, &endPtr);
        if (endPtr == raw || !isfinite(parsed))
            return false;

        while (*endPtr == ' ' || *endPtr == '\t' || *endPtr == '\r' || *endPtr == '\n')
            ++endPtr;
        if (*endPtr != '\0')
            return false;

        outValue = parsed;
        return true;
    }

    outValue = value.as<float>();
    return isfinite(outValue);
}

bool extractCloudSnapshot(JsonObjectConst data,
                          bool& valid,
                          bool& controlReady,
                          bool& hasTemp, float& temp,
                          bool& hasHum, float& hum,
                          bool& hasLight, float& light)
{
    valid = false;
    controlReady = false;
    hasTemp = parseCloudFloat(data["temperature"], temp);
    hasHum = parseCloudFloat(data["humidity"], hum);
    hasLight = parseCloudFloat(data["light_intensity"], light);
    if (!hasLight)
        hasLight = parseCloudFloat(data["lux"], light);

    const bool validTemp = hasTemp && SensorNormalization::normalizeTemperature(temp);
    const bool validHum = hasHum && SensorNormalization::normalizeHumidity(hum);
    const bool validLight = hasLight && SensorNormalization::normalizeLight(light);

    hasTemp = validTemp;
    hasHum = validHum;
    hasLight = validLight;

    if (!hasTemp && !hasHum && !hasLight)
        return false;

    // Respons all-zero biasanya indikasi payload gagal/default dari backend.
    if (hasTemp && hasHum && hasLight && temp == 0.0f && hum == 0.0f && light == 0.0f)
        return false;

    valid = true;
    controlReady = hasTemp && hasHum;
    return true;
}

bool parseThresholdRange(JsonObjectConst item, float& outMin, float& outMax)
{
    float minValue = 0.0f;
    float maxValue = 0.0f;
    if (!parseCloudFloat(item["threshold_min"], minValue) ||
        !parseCloudFloat(item["threshold_max"], maxValue)) {
        return false;
    }

    if (!(minValue == minValue) || !(maxValue == maxValue) || minValue > maxValue)
        return false;

    outMin = minValue;
    outMax = maxValue;
    return true;
}

bool isValidThresholdRangeForName(const char* name, float minValue, float maxValue)
{
    return ThresholdValidation::isRangeValidForName(name, minValue, maxValue);
}

struct ThresholdCandidate
{
    bool present = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    String timestamp;
    long id = LONG_MIN;
};

String getThresholdRecordTimestamp(JsonObjectConst item)
{
    const char* updatedAt = item["updated_at"];
    if (updatedAt && updatedAt[0] != '\0')
        return String(updatedAt);

    const char* createdAt = item["created_at"];
    if (createdAt && createdAt[0] != '\0')
        return String(createdAt);

    return String();
}

bool shouldReplaceThresholdCandidate(const ThresholdCandidate& current,
                                     const String& candidateTimestamp,
                                     long candidateId)
{
    if (!current.present)
        return true;

    if (candidateTimestamp.length() > 0)
    {
        if (current.timestamp.length() == 0)
            return true;
        if (candidateTimestamp > current.timestamp)
            return true;
        if (candidateTimestamp < current.timestamp)
            return false;
    }
    else if (current.timestamp.length() > 0)
    {
        return false;
    }

    return candidateId > current.id;
}

void assignThresholdCandidate(ThresholdCandidate& target,
                              float minValue,
                              float maxValue,
                              const String& timestamp,
                              long id)
{
    target.present = true;
    target.minValue = minValue;
    target.maxValue = maxValue;
    target.timestamp = timestamp;
    target.id = id;
}

bool extractThresholdSnapshot(JsonArrayConst data, CloudThresholdSnapshot& outSnapshot)
{
    ThresholdCandidate tempCandidate = {};
    ThresholdCandidate humCandidate = {};
    ThresholdCandidate lightCandidate = {};

    for (JsonObjectConst item : data)
    {
        const char* name = item["name"];
        float minValue = 0.0f;
        float maxValue = 0.0f;
        if (!parseThresholdRange(item, minValue, maxValue))
            continue;
        if (!isValidThresholdRangeForName(name, minValue, maxValue))
            continue;

        const String timestamp = getThresholdRecordTimestamp(item);
        const long recordId = item["id"].isNull() ? LONG_MIN : item["id"].as<long>();

        if (strcmp(name, "Temperature") == 0)
        {
            if (shouldReplaceThresholdCandidate(tempCandidate, timestamp, recordId))
                assignThresholdCandidate(tempCandidate, minValue, maxValue, timestamp, recordId);
        }
        else if (strcmp(name, "Humidity") == 0)
        {
            if (shouldReplaceThresholdCandidate(humCandidate, timestamp, recordId))
                assignThresholdCandidate(humCandidate, minValue, maxValue, timestamp, recordId);
        }
        else if (strcmp(name, "Light Intensity") == 0)
        {
            if (shouldReplaceThresholdCandidate(lightCandidate, timestamp, recordId))
                assignThresholdCandidate(lightCandidate, minValue, maxValue, timestamp, recordId);
        }
    }

    outSnapshot = CloudThresholdSnapshot{};
    if (tempCandidate.present)
    {
        outSnapshot.tempMin = tempCandidate.minValue;
        outSnapshot.tempMax = tempCandidate.maxValue;
        outSnapshot.hasTemp = true;
    }
    if (humCandidate.present)
    {
        outSnapshot.humMin = humCandidate.minValue;
        outSnapshot.humMax = humCandidate.maxValue;
        outSnapshot.hasHum = true;
    }
    if (lightCandidate.present)
    {
        outSnapshot.lightMin = lightCandidate.minValue;
        outSnapshot.lightMax = lightCandidate.maxValue;
        outSnapshot.hasLight = true;
    }

    outSnapshot.valid = outSnapshot.hasTemp && outSnapshot.hasHum;
    return outSnapshot.valid;
}

bool parseFogStatus(JsonVariantConst value, bool& outStatus)
{
    if (value.isNull())
        return false;

    if (value.is<bool>()) {
        outStatus = value.as<bool>();
        return true;
    }

    if (value.is<int>()) {
        const int fogInt = value.as<int>();
        if (fogInt == 0 || fogInt == 1) {
            outStatus = (fogInt == 1);
            return true;
        }
        return false;
    }

    if (value.is<const char*>()) {
        const char* raw = value.as<const char*>();
        if (!raw)
            return false;

        if (strcmp(raw, "1") == 0 ||
            strcmp(raw, "true") == 0 ||
            strcmp(raw, "TRUE") == 0 ||
            strcmp(raw, "True") == 0) {
            outStatus = true;
            return true;
        }

        if (strcmp(raw, "0") == 0 ||
            strcmp(raw, "false") == 0 ||
            strcmp(raw, "FALSE") == 0 ||
            strcmp(raw, "False") == 0) {
            outStatus = false;
            return true;
        }
    }

    return false;
}

bool formatUtcOffsetHours(float offsetHours, char* out, size_t outLen)
{
    if (!out || outLen == 0)
        return false;

    const float quarterHourFloat = offsetHours * 4.0f;
    const long quarterHours = lroundf(quarterHourFloat);
    if (fabsf(quarterHourFloat - static_cast<float>(quarterHours)) > 0.01f)
        return false;

    const long totalMinutes = quarterHours * 15L;
    const long absMinutes = labs(totalMinutes);
    const char sign = totalMinutes < 0 ? '-' : '+';
    snprintf(out, outLen, "%c%02ld:%02ld", sign, absMinutes / 60L, absMinutes % 60L);
    return true;
}

bool buildEpochFromLocalComponents(int year,
                                   int month,
                                   int day,
                                   int hour,
                                   int minute,
                                   int second,
                                   uint32_t& outEpoch)
{
    if (year < 2023 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
    {
        return false;
    }

    struct tm localTime = {};
    localTime.tm_year = year - 1900;
    localTime.tm_mon = month - 1;
    localTime.tm_mday = day;
    localTime.tm_hour = hour;
    localTime.tm_min = minute;
    localTime.tm_sec = second;
    localTime.tm_isdst = 0;

    const time_t epoch = mktime(&localTime);
    if (epoch <= 1672531200)
        return false;

    struct tm normalized = {};
    localtime_r(&epoch, &normalized);
    if ((normalized.tm_year + 1900) != year ||
        (normalized.tm_mon + 1) != month ||
        normalized.tm_mday != day ||
        normalized.tm_hour != hour ||
        normalized.tm_min != minute ||
        normalized.tm_sec != second)
    {
        return false;
    }

    outEpoch = static_cast<uint32_t>(epoch);
    return true;
}

bool extractIPv4FromText(const String& text, char* out, size_t outLen)
{
    if (!out || outLen == 0)
        return false;

    out[0] = '\0';
    const size_t len = text.length();
    for (size_t start = 0; start < len; ++start)
    {
        if (!isDigit(text[start]))
            continue;

        size_t end = start;
        while (end < len && (isDigit(text[end]) || text[end] == '.'))
            ++end;

        const String candidate = text.substring(start, end);
        int dots = 0;
        int octets = 0;
        bool valid = true;
        int currentValue = -1;
        for (size_t i = 0; i <= candidate.length(); ++i)
        {
            const bool atEnd = (i == candidate.length());
            const char ch = atEnd ? '.' : candidate[i];
            if (isDigit(ch))
            {
                if (currentValue < 0)
                    currentValue = 0;
                currentValue = (currentValue * 10) + (ch - '0');
                if (currentValue > 255)
                {
                    valid = false;
                    break;
                }
            }
            else if (ch == '.')
            {
                if (currentValue < 0)
                {
                    valid = false;
                    break;
                }
                ++octets;
                if (!atEnd)
                    ++dots;
                currentValue = -1;
            }
            else
            {
                valid = false;
                break;
            }
        }

        if (valid && dots == 3 && octets == 4 && candidate != "0.0.0.0")
        {
            strncpy(out, candidate.c_str(), outLen - 1);
            out[outLen - 1] = '\0';
            return true;
        }

        start = end;
    }

    return false;
}
} // namespace

// Constructor
MyNetworkManager::MyNetworkManager(SensorDataManager& data, LCDDisplay& display, RelayController& relayCtrl)
    : sensorData(data), lcd_ref(display), relayController(relayCtrl), modem(SerialAT), gprsClient(modem), gprsSecureClient(modem) {}

// Begin
void MyNetworkManager::begin(const char* token, const char* ta_token, const char* th_url, const char* nd_url, 
                              const char* nd_url_base, const char* post_url, const char* get_url,
                              const char* schedule_url, 
                              const char* gprs_apn, const char* gprs_user, const char* gprs_pass, 
                              const char* sim_pin) {
    cfg_auth_token = token;
    cfg_ta_token = ta_token;
    cfg_th_url = th_url; cfg_nd_url = nd_url; cfg_nd_url_base = nd_url_base;
    cfg_post_url = post_url; cfg_get_url = get_url;
    cfg_schedule_url = schedule_url;
    
    cfg_gprs_apn = gprs_apn; cfg_gprs_user = gprs_user; cfg_gprs_pass = gprs_pass; cfg_sim_pin = sim_pin;
    
    // Set URL Ram
    char worldtime_url_ram[128];
    strncpy_P(worldtime_url_ram, WORLDTIME_URL, sizeof(worldtime_url_ram) - 1);
    worldtime_url_ram[sizeof(worldtime_url_ram) - 1] = '\0';
    cfg_worldtime_url = worldtime_url_ram;

    m_credentialStore.init();
    
    lcd_ref.message(0, 1, "Starting Network...", true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    m_connectionFailures = 0;
    m_retryDelay = MIN_RETRY_DELAY;
    m_currentCredential = nullptr;
    m_scanPending = true;
    setWiFiState(WiFiState::IDLE);
    handleWiFi();
}

// Legacy Begin
void MyNetworkManager::begin(const char* ssid, const char* pwd, const char* token, const char* ta_token,
                              const char* th_url, const char* nd_url, const char* nd_url_base, 
                              const char* post_url, const char* get_url, 
                              const char* schedule_url,
                              const char* gprs_apn, 
                              const char* gprs_user, const char* gprs_pass, const char* sim_pin) {
    cfg_ssid = ssid; cfg_pwd = pwd;
    if (ssid && strlen(ssid) > 0 && !m_credentialStore.hasCredential(ssid))
        m_credentialStore.addCredential(ssid, pwd, false);
    
    begin(token, ta_token, th_url, nd_url, nd_url_base, post_url, get_url, schedule_url, 
          gprs_apn, gprs_user, gprs_pass, sim_pin);
}

void MyNetworkManager::updateApiConfig(const char* token,
                                       const char* ta_token,
                                       const char* th_url,
                                       const char* nd_url,
                                       const char* nd_url_base,
                                       const char* post_url,
                                       const char* get_url,
                                       const char* schedule_url) {
    if (token) cfg_auth_token = token;
    if (ta_token) cfg_ta_token = ta_token;
    if (th_url) cfg_th_url = th_url;
    if (nd_url) cfg_nd_url = nd_url;
    if (nd_url_base) cfg_nd_url_base = nd_url_base;
    if (post_url) cfg_post_url = post_url;
    if (get_url) cfg_get_url = get_url;
    if (schedule_url) cfg_schedule_url = schedule_url;
}

void MyNetworkManager::setUplinkMode(UplinkMode mode) {
    if (static_cast<uint8_t>(mode) > static_cast<uint8_t>(UplinkMode::RELAY))
        mode = UplinkMode::AUTO;

    m_uplinkMode = mode;
    if (m_uplinkMode != UplinkMode::AUTO) {
        m_forceRelayNextCloudAttempt = false;
        m_relayPinnedUntil = 0;
    }

    Serial.printf("[UPLINK] Mode set to %s\n", uplinkModeName(m_uplinkMode));
}

void MyNetworkManager::copyUplinkModeString(char* out, size_t out_len) const {
    copyText(out, out_len, uplinkModeName(m_uplinkMode));
}

void MyNetworkManager::copyActiveUplinkRouteString(char* out, size_t out_len) const {
    copyText(out, out_len, shouldUseRelayForCloudRequest() ? "relay" : "direct");
}

bool MyNetworkManager::shouldUseRelayForCloudRequest() const {
    switch (m_uplinkMode)
    {
    case UplinkMode::DIRECT:
        return false;
    case UplinkMode::RELAY:
        return true;
    case UplinkMode::AUTO:
    default:
        if (m_forceRelayNextCloudAttempt)
            return true;
        if (m_relayPinnedUntil == 0)
            return false;
        return static_cast<int32_t>(millis() - m_relayPinnedUntil) < 0;
    }
}

bool MyNetworkManager::shouldTreatAsCloudFailure(int httpStatusCode, bool wafBlocked) const {
    if (wafBlocked)
        return true;
    if (httpStatusCode < 0)
        return true;
    if (httpStatusCode == 400 || httpStatusCode == 401 || httpStatusCode == 422)
        return false;
    if (httpStatusCode >= 300 && httpStatusCode < 400)
        return true;
    if (httpStatusCode == 403 || httpStatusCode == 408 || httpStatusCode == 425 || httpStatusCode == 429)
        return true;
    return httpStatusCode >= 500;
}

bool MyNetworkManager::shouldFallbackToRelay(const String& originalUrl,
                                             bool usedRelay,
                                             int httpStatusCode,
                                             bool wafBlocked) const {
    return m_uplinkMode == UplinkMode::AUTO &&
           !usedRelay &&
           isRelayCapableUrl(originalUrl) &&
           shouldTreatAsCloudFailure(httpStatusCode, wafBlocked);
}

void MyNetworkManager::activateRelayFallback() {
    if (m_uplinkMode != UplinkMode::AUTO)
        return;

    m_forceRelayNextCloudAttempt = true;
    m_relayPinnedUntil = millis() + RELAY_FALLBACK_PIN_MS;
    Serial.printf("[UPLINK] Direct path failed. Relay pinned for %lu s.\n",
                  RELAY_FALLBACK_PIN_MS / 1000UL);
}

void MyNetworkManager::clearRelayFallback() {
    m_forceRelayNextCloudAttempt = false;
    m_relayPinnedUntil = 0;
    Serial.println("[UPLINK] Direct path recovered. Relay pin cleared.");
}

bool MyNetworkManager::shouldUseTaTokenForUrl(const String& url) const {
    const String normalized = normalizeOriginUrl(url);
    return normalized.startsWith(kTaOriginHttps) || normalized.startsWith(kTaOriginHttp);
}

String MyNetworkManager::normalizeOriginUrl(const String& url) const {
    if (url.startsWith(kRelayAtomicPrefix)) {
        return String(kAtomicOriginHttps) + url.substring(strlen(kRelayAtomicPrefix));
    }
    if (url.startsWith(kRelayTaPrefix)) {
        return String(kTaOriginHttps) + url.substring(strlen(kRelayTaPrefix));
    }
    return url;
}

bool MyNetworkManager::isRelayCapableUrl(const String& url) const {
    const String normalized = normalizeOriginUrl(url);
    return normalized.startsWith(kAtomicOriginHttps) ||
           normalized.startsWith(kAtomicOriginHttp) ||
           normalized.startsWith(kTaOriginHttps) ||
           normalized.startsWith(kTaOriginHttp);
}

String MyNetworkManager::buildRelayUrl(const String& url) const {
    const String normalized = normalizeOriginUrl(url);
    if (normalized.startsWith(kAtomicOriginHttps)) {
        return String(kRelayAtomicPrefix) + normalized.substring(strlen(kAtomicOriginHttps));
    }
    if (normalized.startsWith(kAtomicOriginHttp)) {
        return String(kRelayAtomicPrefix) + normalized.substring(strlen(kAtomicOriginHttp));
    }
    if (normalized.startsWith(kTaOriginHttps)) {
        return String(kRelayTaPrefix) + normalized.substring(strlen(kTaOriginHttps));
    }
    if (normalized.startsWith(kTaOriginHttp)) {
        return String(kRelayTaPrefix) + normalized.substring(strlen(kTaOriginHttp));
    }
    return normalized;
}

String MyNetworkManager::resolveUplinkUrl(const String& url) const {
    const String normalized = normalizeOriginUrl(url);
    if (!shouldUseRelayForCloudRequest())
        return normalized;
    if (!isRelayCapableUrl(normalized))
        return normalized;
    return buildRelayUrl(normalized);
}

// =================================================================================
//   CORE HTTP FUNCTION (WIFI) - STABLE FIX
// =================================================================================
bool MyNetworkManager::_performHttpRequestWiFi(const String& url_in,
                                               const char* method,
                                               const char* payload,
                                               bool needsAuth,
                                               bool useTaToken,
                                               int& httpStatusCode,
                                               bool& wafBlocked,
                                               std::function<bool(JsonDocument&)> cb,
                                               uint16_t timeoutMs) {
    
    // 1. Cek Memori (Safety)
    if (ESP.getFreeHeap() < 25000) {
        Serial.printf("[MEM] Low RAM (%d). Yielding...\n", ESP.getFreeHeap());
        delay(200);
    }

    HTTPClient http;
    String url = url_in;
    wafBlocked = false;
    const unsigned long requestStartMs = millis();
    const String loggedUrl = sanitizeUrlForLog(url);

    // 2. Client Setup (Stack Allocation - Safer)
    WiFiClientSecure secureClient;
    WiFiClient basicClient;

    bool isSecure = url.startsWith("https://");
    const uint32_t clientTimeoutSeconds = timeoutMsToClientSeconds(timeoutMs);

    if (isSecure) {
        secureClient.setInsecure();
        secureClient.setTimeout(clientTimeoutSeconds);
    } else {
        basicClient.setTimeout(clientTimeoutSeconds);
    }

    // 3. Begin Connection
    bool connected = false;
    if (isSecure) {
        connected = http.begin(secureClient, url);
    } else {
        connected = http.begin(basicClient, url);
    }

    if (!connected) {
        Serial.println("[HTTP] Unable to init connection");
        httpStatusCode = -1;
        return false;
    }

    // [FIX UTAMA] Gunakan HTTP 1.0 untuk mencegah EOF Error (-29312)
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(timeoutMs);
    
    // Headers
    char userAgent[kGatewayUserAgentLen];
    buildGatewayUserAgent(userAgent, sizeof(userAgent));
    char deviceId[kGatewayDeviceIdLen];
    buildGatewayDeviceId(deviceId, sizeof(deviceId));
    if (needsAuth) {
        String tokenToUse = useTaToken && cfg_ta_token.length() > 0 ? cfg_ta_token : cfg_auth_token;
        http.addHeader("Authorization", "Bearer " + tokenToUse);
    }
    if (payload && strlen(payload) > 0) http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", userAgent);
    http.addHeader("X-Device-ID", deviceId);
    http.addHeader("Connection", "close"); // Force close

    // 4. Execute
    esp_task_wdt_reset();
    const unsigned long requestDispatchMs = millis();
    if (strcmp(method, "GET") == 0) httpStatusCode = http.GET();
    else if (strcmp(method, "POST") == 0) httpStatusCode = http.POST(payload ? payload : "");
    const unsigned long requestElapsedMs = millis() - requestDispatchMs;

    bool callbackOk = false;

    String responseBody;
    
    if (httpStatusCode >= 200 && httpStatusCode < 300) {
        responseBody = http.getString();
        Serial.printf("[HTTP][WiFi] OK %d in %lums body=%uB | URL: %s\n",
                      httpStatusCode,
                      requestElapsedMs,
                      static_cast<unsigned>(responseBody.length()),
                      loggedUrl.c_str());
        if (responseBody.length() > 0 && responseBodyIndicatesWafBlock(responseBody.c_str())) {
            wafBlocked = true;
            httpStatusCode = 403;
            Serial.printf("[HTTP] WAF block detected | URL: %s\n", loggedUrl.c_str());
        } else if (cb) {
            if (responseBody.length() > 0) {
                DeserializationError err = deserializeJson(jsonDoc, responseBody);
                if (!err) {
                    callbackOk = cb(jsonDoc);
                } else {
                    Serial.print("[JSON] Deserialization failed: ");
                    Serial.println(err.c_str());
                }
            } else {
                Serial.println("[HTTP] Response Empty");
            }
        } else {
            callbackOk = true;
        }
    } else {
        if (httpStatusCode >= 0) {
            responseBody = http.getString();
            if (responseBody.length() > 0 && responseBodyIndicatesWafBlock(responseBody.c_str())) {
                wafBlocked = true;
                Serial.printf("[HTTP] WAF block detected | URL: %s\n", loggedUrl.c_str());
            }
        }
        if (httpStatusCode < 0) {
            if (httpStatusCode == HTTPC_ERROR_READ_TIMEOUT) {
                Serial.printf("[HTTP][WiFi] Response header timeout after %lums (budget=%u ms, secure=%s, rssi=%d, heap=%u) | URL: %s\n",
                              requestElapsedMs,
                              timeoutMs,
                              isSecure ? "YES" : "NO",
                              WiFi.RSSI(),
                              ESP.getFreeHeap(),
                              loggedUrl.c_str());
            }
            if (isSecure)
                logTlsClientLastError(secureClient, "request", url);
            String errText = HTTPClient::errorToString(httpStatusCode);
            Serial.printf("[HTTP] Fail! Code: %d (%s) | URL: %s\n", httpStatusCode, errText.c_str(), loggedUrl.c_str());
        } else {
            Serial.printf("[HTTP] Fail! Code: %d | URL: %s\n", httpStatusCode, loggedUrl.c_str());
        }
    }

    // 5. Cleanup
    http.end();
    // Tidak perlu delete manual, stack variable akan otomatis dihapus

    if (!callbackOk && httpStatusCode == HTTPC_ERROR_READ_TIMEOUT) {
        const unsigned long totalElapsedMs = millis() - requestStartMs;
        Serial.printf("[HTTP][WiFi] Request ended in read-timeout after %lums total | URL: %s\n",
                      totalElapsedMs,
                      loggedUrl.c_str());
    }

    delay(100);
    return callbackOk;
}

// =================================================================================
//   CORE HTTP FUNCTION (GPRS)
// =================================================================================
bool MyNetworkManager::_performHttpRequestGPRS(const String& url_in,
                                               const char* method,
                                               const char* payload,
                                               bool needsAuth,
                                               bool useTaToken,
                                               int& httpStatusCode,
                                               bool& wafBlocked,
                                               std::function<bool(JsonDocument&)> cb,
                                               uint16_t timeoutMs) {
    String url = url_in; 
    wafBlocked = false;
    const String loggedUrl = sanitizeUrlForLog(url);
    int pEnd = url.indexOf("://");
    int hStart = (pEnd == -1) ? 0 : pEnd + 3;
    int pStart = url.indexOf('/', hStart);
    String host = (pStart == -1) ? url.substring(hStart) : url.substring(hStart, pStart);
    String path = (pStart == -1) ? String("/") : url.substring(pStart);
    
    bool isSecure = url.startsWith("https");
    int port = isSecure ? 443 : 80;

    gprsSecureClient.setTimeout(timeoutMs);
    gprsClient.setTimeout(timeoutMs);

    bool connected = false;
    if(isSecure) connected = gprsSecureClient.connect(host.c_str(), port);
    else connected = gprsClient.connect(host.c_str(), port);

    if (!connected) {
        markGprsSessionDown();
        return false;
    }
    
    Client* client = isSecure ? (Client*)&gprsSecureClient : (Client*)&gprsClient;
    String req = String(method) + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\n"; // Changed to 1.0
    char userAgent[kGatewayUserAgentLen];
    buildGatewayUserAgent(userAgent, sizeof(userAgent));
    char deviceId[kGatewayDeviceIdLen];
    buildGatewayDeviceId(deviceId, sizeof(deviceId));
    
    if (needsAuth) {
        String tokenToUse = useTaToken && cfg_ta_token.length() > 0 ? cfg_ta_token : cfg_auth_token;
        req += "Authorization: Bearer " + tokenToUse + "\r\n";
    }
    if (payload && strlen(payload) > 0) {
        req += "Content-Type: application/json\r\nContent-Length: " + String(strlen(payload)) + "\r\n";
    }
    req += "User-Agent: " + String(userAgent) + "\r\n";
    req += "X-Device-ID: " + String(deviceId) + "\r\n";
    req += "Connection: close\r\n\r\n";
    if (payload && strlen(payload) > 0) req += payload;
    
    client->print(req);
    
    unsigned long timeout = millis();
    while (client->connected() && !client->available()) {
        if (millis() - timeout > timeoutMs) {
            client->stop();
            markGprsSessionDown();
            return false;
        }
        delay(10);
        esp_task_wdt_reset();
    }
    
    String response = client->readString();
    client->stop();
    
    int sPos = response.indexOf(" ");
    if (sPos != -1) {
        httpStatusCode = response.substring(sPos+1, sPos+4).toInt();
    } else {
        httpStatusCode = -1;
    }

    int bodyPos = response.indexOf("\r\n\r\n");
    String body = bodyPos != -1 ? response.substring(bodyPos + 4) : String();
    if (body.length() > 0 && responseBodyIndicatesWafBlock(body.c_str())) {
        wafBlocked = true;
        if (httpStatusCode >= 200 && httpStatusCode < 300)
            httpStatusCode = 403;
        Serial.printf("[HTTP][GPRS] WAF block detected | URL: %s\n", loggedUrl.c_str());
        return false;
    }

    if (httpStatusCode >= 200 && httpStatusCode < 300) {
        Serial.printf("[HTTP][GPRS] OK %d body=%uB | URL: %s\n",
                      httpStatusCode,
                      static_cast<unsigned>(body.length()),
                      loggedUrl.c_str());
        markGprsSessionUp(m_gprsLocalIp[0] != '\0' ? m_gprsLocalIp : nullptr);
        if (cb) {
            if (bodyPos == -1 || body.length() == 0)
                return false;
            DeserializationError err = deserializeJson(jsonDoc, body);
            return !err && cb(jsonDoc);
        }
        return true;
    }
    return false;
}

// --- DISPATCHER ---
bool MyNetworkManager::_performHttpRequest(const char* url, const char* method, const char* apiType, const char* payload, std::function<bool(JsonDocument&)> cb, bool needsAuth, uint16_t timeoutMs) {
    if (!isConnected() || !url || url[0] == '\0')
        return false;

    const String originalUrl = normalizeOriginUrl(String(url));
    const bool useTaToken = shouldUseTaTokenForUrl(originalUrl);
    const bool relayCapable = isRelayCapableUrl(originalUrl);
    bool requestUsedRelay = relayCapable && shouldUseRelayForCloudRequest();
    if (requestUsedRelay && m_uplinkMode == UplinkMode::AUTO && m_forceRelayNextCloudAttempt) {
        m_forceRelayNextCloudAttempt = false;
    }

    std::function<bool(bool)> runAttempt = [&](bool useRelay) -> bool {
        jsonDoc.clear();
        int httpCode = -1;
        bool wafBlocked = false;
        bool ok = false;
        const String effectiveUrl = useRelay ? buildRelayUrl(originalUrl) : originalUrl;

        if (isWiFiConnected()) {
            ok = _performHttpRequestWiFi(effectiveUrl,
                                         method,
                                         payload,
                                         needsAuth,
                                         useTaToken,
                                         httpCode,
                                         wafBlocked,
                                         cb,
                                         timeoutMs);
        } else if (isGprsConnected()) {
            ok = _performHttpRequestGPRS(effectiveUrl,
                                         method,
                                         payload,
                                         needsAuth,
                                         useTaToken,
                                         httpCode,
                                         wafBlocked,
                                         cb,
                                         timeoutMs);
        }

        const bool success = (httpCode >= 200 && httpCode < 300 && ok);
        if (!success && shouldFallbackToRelay(originalUrl, useRelay, httpCode, wafBlocked)) {
            activateRelayFallback();
            const String reason = describeHttpFailure(httpCode, wafBlocked);
            Serial.printf("[UPLINK] [%s] DIRECT FAIL: %s (%d) -> retry RELAY\n",
                          apiType,
                          reason.c_str(),
                          httpCode);
            return runAttempt(true);
        }

        if (success && m_uplinkMode == UplinkMode::AUTO && !useRelay &&
            (m_relayPinnedUntil != 0 || m_forceRelayNextCloudAttempt)) {
            clearRelayFallback();
        }

        if (httpCode != 200 && httpCode != -1) {
            const String reason = describeHttpFailure(httpCode, wafBlocked);
            Serial.printf("[%s] FAIL: %s (%d) via %s\n",
                          apiType,
                          reason.c_str(),
                          httpCode,
                          useRelay ? "relay" : "direct");
        }

        return success;
    };

    return runAttempt(requestUsedRelay);
}

// =================================================================================
//   API IMPLEMENTATIONS
// =================================================================================

bool MyNetworkManager::fetchThresholds(CloudThresholdSnapshot& outSnapshot, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = kThresholdFetchTimeoutMs;
    outSnapshot = CloudThresholdSnapshot{};
    String dynamicUrl = cfg_th_url.length() > 0 ? cfg_th_url : String(TH_URL);
    int q = dynamicUrl.indexOf("gh_id=");
    if (q != -1) {
        int vStart = q + 6;
        int vEnd = dynamicUrl.indexOf('&', vStart);
        if (vEnd == -1) vEnd = dynamicUrl.length();
        dynamicUrl = dynamicUrl.substring(0, vStart) + String(GH_ID_CONFIG) + dynamicUrl.substring(vEnd);
    } else {
        dynamicUrl += String(GH_ID_CONFIG);
    }
    return _performHttpRequest(dynamicUrl.c_str(), "GET", "TH", nullptr, [&](JsonDocument& d) {
        JsonArray data = d["data"].as<JsonArray>();
        if (!data) return false;
        return extractThresholdSnapshot(data, outSnapshot);
    }, true, timeoutMs);
}

bool MyNetworkManager::fetchNodeData(CloudSensorSnapshot& outSnapshot, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    outSnapshot = CloudSensorSnapshot{};
    return _performHttpRequest(cfg_nd_url.c_str(), "GET", "ND", nullptr, [&](JsonDocument& d) {
        JsonObjectConst data = d["data"].as<JsonObjectConst>();
        if (!data) return false;

        if (!extractCloudSnapshot(data,
                                  outSnapshot.valid,
                                  outSnapshot.controlReady,
                                  outSnapshot.hasTemp,
                                  outSnapshot.temp,
                                  outSnapshot.hasHum,
                                  outSnapshot.hum,
                                  outSnapshot.hasLight,
                                  outSnapshot.light)) {
            Serial.println("[ND] Invalid cloud payload ignored; keeping last cloud values.");
            return false;
        }
        return true;
    }, true, timeoutMs);
}
	
bool MyNetworkManager::fetchSchedules(CloudScheduleSnapshot& outSnapshot, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    outSnapshot = CloudScheduleSnapshot{};
    auto parseSchedules = [&](JsonDocument& d) {
        if (d["success"] == true) {
            JsonArray schedules = d["schedules"].as<JsonArray>();
            if (schedules) {
                ScheduleConfig def = {0, false, 0, 0, 0, 0, '2', '2', '2'};
                for (int i = 0; i < MAX_SCHEDULES; ++i)
                    outSnapshot.schedules[i] = def;

                int idx = 0;
                for (JsonObject s : schedules) {
                    if (idx >= MAX_SCHEDULES) break;

                    if (!ScheduleValidation::parseScheduleConfig(s, outSnapshot.schedules[idx])) {
                        Serial.println("[SCHED] Invalid active schedule payload ignored.");
                        return false;
                    }
                    idx++;
                }
                outSnapshot.valid = true;
                return true;
            }
        }
        return false;
    };

    String url = String(cfg_schedule_url);
    url += (url.indexOf('?') == -1) ? "?" : "&";
    url += "gh_id=" + String(GH_ID_CONFIG);

    if (_performHttpRequest(url.c_str(), "GET", "SCHED", nullptr, parseSchedules, true, timeoutMs))
        return true;

    String tokenToUse = shouldUseTaTokenForUrl(cfg_schedule_url)
                            ? (cfg_ta_token.length() > 0 ? cfg_ta_token : cfg_auth_token)
                            : cfg_auth_token;
    if (tokenToUse.length() == 0)
        return false;

    String legacyUrl = url + "&token=" + tokenToUse;
    Serial.println("[SCHED] Header auth path failed; retrying legacy query-token path.");
    return _performHttpRequest(legacyUrl.c_str(), "GET", "SCHED", nullptr, parseSchedules, true, timeoutMs);
}
	
bool MyNetworkManager::fetchBundleForGreenhouse(int greenhouseId,
                                                CloudSensorSnapshot& sensorOut,
                                                CloudThresholdSnapshot& thresholdOut,
                                                CloudFogSnapshot* fogOut,
                                                Stream& output,
                                                uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    sensorOut = CloudSensorSnapshot{};
    thresholdOut = CloudThresholdSnapshot{};
    if (fogOut)
        *fogOut = CloudFogSnapshot{};
    output.printf("Syncing GH ID: %d...\n", greenhouseId);
    String sUrl = String(cfg_nd_url_base) + String(greenhouseId);
    String tUrl = cfg_th_url;
    int q = tUrl.indexOf("?gh_id=");
    if (q != -1) tUrl = tUrl.substring(0, q + 7) + String(greenhouseId);
    else tUrl += String(greenhouseId); 

    bool sOk = _performHttpRequest(sUrl.c_str(), "GET", "MAN_ND", nullptr, [&](JsonDocument& d) {
        JsonObjectConst data = d["data"].as<JsonObjectConst>();
        if (!data) return false;

        if (!extractCloudSnapshot(data,
                                  sensorOut.valid,
                                  sensorOut.controlReady,
                                  sensorOut.hasTemp,
                                  sensorOut.temp,
                                  sensorOut.hasHum,
                                  sensorOut.hum,
                                  sensorOut.hasLight,
                                  sensorOut.light)) {
            output.println("[SKIP] Invalid sensor payload, old cloud data preserved.");
            return false;
        }
        return true;
    }, true, timeoutMs);
		
    delay(200);
		
    bool tOk = _performHttpRequest(tUrl.c_str(), "GET", "MAN_TH", nullptr, [&](JsonDocument& d) {
        JsonArray data = d["data"].as<JsonArray>();
        if (!data) return false;
        return extractThresholdSnapshot(data, thresholdOut);
    }, true, timeoutMs);

#if GH_ID_CONFIG == 2
    delay(200);
    const String cUrl = String(CAMERA_STATUS_URL_BASE) + String(greenhouseId);
    const bool cOk = _performHttpRequest(cUrl.c_str(), "GET", "MAN_CAM", nullptr, [&](JsonDocument& d) {
        JsonObject data = d["data"];
        if (data.isNull())
            return false;
        if (!fogOut)
            return false;
        if (!parseFogStatus(data["isFoggy"], fogOut->foggy))
            return false;
        fogOut->valid = true;
        return true;
    }, true, timeoutMs);
#else
    const bool cOk = true;
#endif

    return sOk && tOk && cOk;
}
	
bool MyNetworkManager::postSingleDeviceStatus(int ghId, const char* key, bool status, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    postJsonDoc.clear(); postJsonDoc["gh_id"] = ghId; postJsonDoc[key] = status ? 1 : 0;
    String payload; serializeJson(postJsonDoc, payload);
    return _performHttpRequest(cfg_post_url.c_str(), "POST", "DEV_ST_P", payload.c_str(), nullptr, true, timeoutMs);
}

bool MyNetworkManager::getDeviceStatus(int ghId, bool& exh, bool& deh, bool& blw, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    return _performHttpRequest(cfg_get_url.c_str(), "GET", "DEV_ST_G", nullptr, [&](JsonDocument& doc) {
        JsonObject data = doc["data"];
        if (data && data["gh_id"].as<String>().toInt() == ghId) {
            exh = data["exhaust_status"].as<String>().toInt() == 1;
            deh = data["dehumidifier_status"].as<String>().toInt() == 1;
            blw = data["blower_status"].as<String>().toInt() == 1;
            return true;
        }
        return false;
    }, true, timeoutMs);
}

uint32_t MyNetworkManager::fetchHttpTimeEpoch(uint16_t timeoutMs) {
    HttpTimeInfo info;
    if (!fetchHttpTimeInfo(info, timeoutMs))
        return 0;
    return info.epoch;
}

bool MyNetworkManager::fetchHttpTimeInfo(HttpTimeInfo& outInfo, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;

    outInfo = HttpTimeInfo{};
    return _performHttpRequest(cfg_worldtime_url.c_str(), "GET", "WT", nullptr, [&](JsonDocument& d) {
        const char* timezone = "";
        if (!d["timezone"].isNull()) {
            timezone = d["timezone"].as<const char*>();
        } else if (!d["timeZone"].isNull()) {
            timezone = d["timeZone"].as<const char*>();
        }
        if (!timezone) {
            timezone = "";
        }

        const char* utcOffset = "";
        if (!d["utc_offset"].isNull()) {
            utcOffset = d["utc_offset"].as<const char*>();
        } else if (!d["utcOffset"].isNull()) {
            utcOffset = d["utcOffset"].as<const char*>();
        }
        if (!utcOffset) {
            utcOffset = "";
        }

        if (!d["unixtime"].isNull()) {
            outInfo.epoch = d["unixtime"].as<uint32_t>();
        } else if (!d["year"].isNull() &&
                   !d["month"].isNull() &&
                   !d["day"].isNull() &&
                   !d["hour"].isNull() &&
                   !d["minute"].isNull() &&
                   (!d["seconds"].isNull() || !d["second"].isNull())) {
            const int year = d["year"].as<int>();
            const int month = d["month"].as<int>();
            const int day = d["day"].as<int>();
            const int hour = d["hour"].as<int>();
            const int minute = d["minute"].as<int>();
            int second = 0;
            if (!d["seconds"].isNull()) {
                second = d["seconds"].as<int>();
            } else if (!d["second"].isNull()) {
                second = d["second"].as<int>();
            }
            if (!buildEpochFromLocalComponents(year, month, day, hour, minute, second, outInfo.epoch))
                return false;
        }

        if (utcOffset[0] == '\0' && strcmp(timezone, GATEWAY_TIMEZONE_NAME) == 0) {
            utcOffset = GATEWAY_TIMEZONE_OFFSET;
        }

        strncpy(outInfo.timezone, timezone, sizeof(outInfo.timezone) - 1);
        outInfo.timezone[sizeof(outInfo.timezone) - 1] = '\0';
        strncpy(outInfo.utcOffset, utcOffset, sizeof(outInfo.utcOffset) - 1);
        outInfo.utcOffset[sizeof(outInfo.utcOffset) - 1] = '\0';
        return outInfo.epoch > 1672531200UL &&
               outInfo.timezone[0] != '\0' &&
               outInfo.utcOffset[0] != '\0';
    }, false, timeoutMs);
}

bool MyNetworkManager::fetchModemTimeInfo(ModemTimeInfo& outInfo) {
    outInfo = ModemTimeInfo{};

    if (!isGprsConnected())
        return false;

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    float timezoneHours = 0.0f;
    if (!modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &timezoneHours))
        return false;
    if (!isfinite(timezoneHours) || timezoneHours < -12.0f || timezoneHours > 14.0f)
        return false;

    if (!buildEpochFromLocalComponents(year, month, day, hour, minute, second, outInfo.epoch))
        return false;

    if (!formatUtcOffsetHours(timezoneHours, outInfo.utcOffset, sizeof(outInfo.utcOffset)))
        return false;

    return true;
}

// --- QoS ---
QoSMetrics MyNetworkManager::runSingleUrlQoS(String targetName, String url, const char* method, const char* payload, int samples, Stream& output) {
    QoSMetrics results = {0, 0, 0, 0, "N/A", targetName};
    output.printf(" -> Testing: %s ... ", targetName.c_str());
    if (!isConnected()) { output.println("[Error: No Net]"); return results; }

    int successCount = 0; unsigned long totalDuration = 0; unsigned long totalBytes = 0;
    unsigned long minLat = 999999; unsigned long maxLat = 0;
    
    for(int i=0; i<samples; i++){
        unsigned long startTick = millis();
        bool ok = false;
        int dummyCode = 0;
        bool wafBlocked = false;
        String normalizedUrl = normalizeOriginUrl(url);
        String effectiveUrl = resolveUplinkUrl(normalizedUrl);
        bool useTaToken = shouldUseTaTokenForUrl(normalizedUrl);

        if(isWiFiConnected()) ok = _performHttpRequestWiFi(effectiveUrl, method, payload, true, useTaToken, dummyCode, wafBlocked, nullptr, HTTP_REQUEST_TIMEOUT_MS);
        else if(isGprsConnected()) ok = _performHttpRequestGPRS(effectiveUrl, method, payload, true, useTaToken, dummyCode, wafBlocked, nullptr, HTTP_REQUEST_TIMEOUT_MS);

        unsigned long duration = millis() - startTick;
        if (ok) {
            successCount++; totalDuration += duration;
            totalBytes += 1024;
            if (duration < minLat) minLat = duration;
            if (duration > maxLat) maxLat = duration;
        }
        delay(200); esp_task_wdt_reset();
    }

    results.packetLossPercent = ((float)(samples - successCount) / samples) * 100.0;
    if (successCount > 0) {
        results.avgLatencyMs = (float)totalDuration / successCount;
        if (totalDuration > 0) {
            float totalBits = (float)totalBytes * 8.0;
            float totalSeconds = (float)totalDuration / 1000.0;
            results.throughputKbps = (totalBits / totalSeconds) / 1000.0; 
        }
    }
    if (successCount > 1) results.jitterMs = (float)(maxLat - minLat);
    if (results.packetLossPercent == 0 && results.avgLatencyMs < 400) results.score = "Excellent";
    else if (results.packetLossPercent < 10 && results.avgLatencyMs < 800) results.score = "Good";
    else if (results.packetLossPercent < 20 && results.avgLatencyMs < 1500) results.score = "Fair";
    else results.score = "Poor";
    
    output.println("DONE");
    char buf[128];
    snprintf(buf, sizeof(buf), "    Lat: %.0fms | Loss: %.0f%% | Spd: %.2f Kbps | Score: %s", 
        results.avgLatencyMs, results.packetLossPercent, results.throughputKbps, results.score.c_str());
    output.println(buf); output.println("");
    return results;
}

void MyNetworkManager::qosCheck_Threshold(Stream& output) { runSingleUrlQoS("Threshold", cfg_th_url, "GET", nullptr, 3, output); }
void MyNetworkManager::qosCheck_NodeData(Stream& output) { runSingleUrlQoS("Node Data", cfg_nd_url, "GET", nullptr, 3, output); }
void MyNetworkManager::qosCheck_DeviceStatusGet(Stream& output) { runSingleUrlQoS("Dev Status", cfg_get_url, "GET", nullptr, 3, output); }
void MyNetworkManager::qosCheck_DeviceStatusPost(int ghId, Stream& output) { String p="{\"gh_id\":"+String(ghId)+"}"; runSingleUrlQoS("Dev Post", cfg_post_url, "POST", p.c_str(), 3, output); }
void MyNetworkManager::qosCheck_Firmware(Stream& output) { String u=String(FW_UPDATE_URL) + String(FW_VERSION_ID); runSingleUrlQoS("Firmware", u, "GET", nullptr, 3, output); }

// --- CONNECTION ---
bool MyNetworkManager::connectWiFi() {
    if (WiFi.status() == WL_CONNECTED)
        return true;

    lcd_ref.message(0, 1, "Queue WiFi...", true);
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.disconnect(false, true);
    m_manualSwitchPending = false;
    m_manualSwitchResultReady = false;
    m_manualSwitchResult = {};
    m_currentCredential = nullptr;
    m_scanPending = true;
    m_connectionFailures = 0;
    m_retryDelay = MIN_RETRY_DELAY;
    m_stateStartTime = millis();
    setWiFiState(WiFiState::IDLE);
    return false;
}
bool MyNetworkManager::connectSpecificWiFi(const char* ssid, const char* password, bool saveCredential, bool assumeHidden) {
    return requestSpecificWiFiConnect(ssid, password, saveCredential, assumeHidden);
}

bool MyNetworkManager::requestSpecificWiFiConnect(const char* ssid, const char* password, bool saveCredential, bool assumeHidden) {
    if (!ssid || strlen(ssid) == 0)
        return false;
    if (m_manualSwitchPending)
        return false;

    String targetSsid = String(ssid);
    targetSsid.trim();
    if (targetSsid.length() == 0)
        return false;

    m_manualCredential.clear();
    strncpy(m_manualCredential.ssid, targetSsid.c_str(), WIFI_SSID_MAX_LEN - 1);
    m_manualCredential.ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
    if (password)
        strncpy(m_manualCredential.password, password, WIFI_PASS_MAX_LEN - 1);
    m_manualCredential.password[WIFI_PASS_MAX_LEN - 1] = '\0';
    m_manualCredential.isHidden = assumeHidden;
    m_manualCredential.isBuiltIn = false;
    m_manualCredential.isAvailable = false;
    m_manualCredential.lastRssi = -100;

    m_manualSwitchResult = {};
    m_manualSwitchResult.requestedSsid = targetSsid;
    m_manualSwitchResult.forceHidden = assumeHidden;
    m_manualSwitchSaveCredential = saveCredential;
    m_manualSwitchPending = true;
    m_manualSwitchResultReady = false;
    m_manualSwitchAttempt = 0;
    m_manualSwitchMaxAttempts = assumeHidden ? 3 : 2;
    m_hiddenRetryActive = false;
    m_hiddenRetryShouldPersist = false;
    m_hiddenRetryCredential.clear();
    m_currentCredential = nullptr;
    m_scanPending = true;

    Serial.printf("[WIFI] Queued manual switch to '%s' hidden=%s save=%s\n",
                  targetSsid.c_str(),
                  assumeHidden ? "YES" : "NO",
                  saveCredential ? "YES" : "NO");

    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == targetSsid)
    {
        finishManualWiFiConnect(true);
        return true;
    }

    WiFi.scanDelete();
    WiFi.disconnect(false, true);
    m_stateStartTime = millis();
    setWiFiState(WiFiState::IDLE);
    return true;
}

bool MyNetworkManager::consumeSpecificWiFiConnectResult(SpecificWiFiConnectResult& outResult) {
    if (!m_manualSwitchResultReady)
        return false;

    outResult = m_manualSwitchResult;
    m_manualSwitchResultReady = false;
    m_manualSwitchResult = {};
    return true;
}
bool MyNetworkManager::connectMobile() { 
    if (!gprs_enabled) {
        markGprsSessionDown();
        return false;
    }
    if (isGprsConnected())
        return true;

    lcd_ref.message(0, 1, "Queue GPRS...", true);
    WiFi.scanDelete();
    WiFi.disconnect(false, true);
    m_manualSwitchPending = false;
    m_manualSwitchResultReady = false;
    m_manualSwitchResult = {};
    m_hiddenRetryActive = false;
    m_hiddenRetryShouldPersist = false;
    m_hiddenRetryCredential.clear();
    m_currentCredential = nullptr;
    m_scanPending = false;
    m_connectionFailures = MAX_FAILURES_BEFORE_GPRS;
    m_retryDelay = MIN_RETRY_DELAY;
    m_stateStartTime = 0;
    markGprsSessionDown();
    setWiFiState(WiFiState::FALLBACK_GPRS_WAIT);
    return false;
}
bool MyNetworkManager::isConnected() { return isWiFiConnected() || isGprsConnected(); }
bool MyNetworkManager::isWiFiConnected() { return WiFi.status() == WL_CONNECTED; }
bool MyNetworkManager::isGprsConnected() {
    if (!(m_gprsSessionUp && m_gprsNetworkAttached))
        return false;
    if (m_lastGprsVerifiedUpMs == 0)
        return false;
    return (millis() - m_lastGprsVerifiedUpMs) <= kGprsVerifiedMaxAgeMs;
}
int MyNetworkManager::getSignalQuality() { if (isGprsConnected()) return m_lastGprsSignalQuality; if (isWiFiConnected()) return WiFi.RSSI(); return 0; }
String MyNetworkManager::getActiveSSID() { return isWiFiConnected() ? WiFi.SSID() : "N/A"; }
String MyNetworkManager::getNetworkTimeString() {
    ModemTimeInfo info;
    if (!fetchModemTimeInfo(info))
        return "";

    time_t raw = static_cast<time_t>(info.epoch);
    struct tm localTime = {};
    localtime_r(&raw, &localTime);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return String(buffer) + " " + info.utcOffset;
}
bool MyNetworkManager::addWiFiCredential(const char* ssid, const char* password, bool hidden) { return m_credentialStore.addCredential(ssid, password, hidden); }
bool MyNetworkManager::removeWiFiCredential(const char* ssid) { return m_credentialStore.removeCredential(ssid); }
void MyNetworkManager::triggerRescan() { m_scanPending = true; }
void MyNetworkManager::handleWiFi(uint16_t controlBudgetMs) {
    if (m_manualSwitchPending) {
        handleManualWiFiConnect();
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (m_wifiState != WiFiState::CONNECTED) {
            m_currentCredential = nullptr;
            m_connectionFailures = 0;
            m_retryDelay = MIN_RETRY_DELAY;
            markGprsSessionDown();
            pinMode(MODEM_POWER_ON, OUTPUT);
            digitalWrite(MODEM_POWER_ON, LOW);
            setWiFiState(WiFiState::CONNECTED);
        }
        return;
    }

    if (m_scanPending && m_wifiState != WiFiState::SCANNING && m_wifiState != WiFiState::CONNECTING) {
        startScan();
        return;
    }

    switch (m_wifiState) {
    case WiFiState::IDLE:
        startScan();
        break;
    case WiFiState::SCANNING:
        processScanResults();
        break;
    case WiFiState::CONNECTING:
        handleConnecting();
        break;
    case WiFiState::RETRY_DELAY:
        handleRetryDelay();
        break;
    case WiFiState::FALLBACK_GPRS_WAIT:
    case WiFiState::FALLBACK_GPRS_MODEM_INIT:
    case WiFiState::FALLBACK_GPRS_SIM_READY:
    case WiFiState::FALLBACK_GPRS_NETWORK_ATTACH:
    case WiFiState::FALLBACK_GPRS_CONNECTING:
        handleGprsFallback(controlBudgetMs);
        break;
    case WiFiState::CONNECTED:
    default:
        startScan();
        break;
    }
}
void MyNetworkManager::handleManualWiFiConnect() {
    if (!m_manualSwitchPending)
        return;

    const String targetSsid(m_manualCredential.ssid);
    if (WiFi.status() == WL_CONNECTED) {
        const String connectedNow = WiFi.SSID();
        if (connectedNow == targetSsid) {
            finishManualWiFiConnect(true);
            return;
        }
    }

    if (isGprsFallbackState(m_wifiState)) {
        setWiFiState(WiFiState::IDLE);
    }

    switch (m_wifiState) {
    case WiFiState::SCANNING:
    {
        const int scanResult = WiFi.scanComplete();
        if (scanResult == WIFI_SCAN_RUNNING) {
            if (millis() - m_stateStartTime > CONNECT_TIMEOUT) {
                Serial.printf("[WIFI] Manual scan timeout for '%s'\n", targetSsid.c_str());
                WiFi.scanDelete();
                m_credentialStore.noteScanFailure(-1);
                m_lastScanCompletedMs = millis();
                captureManualScanResult(-1);
                startManualWiFiAttempt();
            }
            return;
        }

        if (scanResult >= 0) {
            m_credentialStore.updateFromScan(scanResult);
        } else {
            m_credentialStore.noteScanFailure(scanResult);
        }
        m_lastScanCompletedMs = millis();
        captureManualScanResult(scanResult);
        WiFi.scanDelete();
        startManualWiFiAttempt();
        return;
    }
    case WiFiState::CONNECTING:
        handleConnecting();
        return;
    case WiFiState::RETRY_DELAY:
        handleRetryDelay();
        return;
    case WiFiState::CONNECTED:
        WiFi.disconnect(false, true);
        m_currentCredential = nullptr;
        setWiFiState(WiFiState::IDLE);
        return;
    case WiFiState::IDLE:
    default:
        break;
    }

    if (m_scanPending || m_manualSwitchAttempt == 0) {
        startScan();
    } else {
        startManualWiFiAttempt();
    }
}
void MyNetworkManager::disconnectAll() {
    WiFi.scanDelete();
    WiFi.disconnect(true);
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, HIGH);
    SerialAT.begin(115200, SERIAL_8N1, GSM_RX, GSM_TX);
    if (modem.testAT(kGprsProbeAtTimeoutMs)) {
        runModemAtCommand("+CIPSHUT", kGprsLongCommandTimeoutMs);
        runModemAtCommand("+SAPBR=0,1", kGprsMediumCommandTimeoutMs);
        runModemAtCommand("+CGACT=0,1", kGprsMediumCommandTimeoutMs);
        runModemAtCommand("+CGATT=0", kGprsMediumCommandTimeoutMs);
    }
    markGprsSessionDown();
    digitalWrite(MODEM_POWER_ON, LOW);
    m_hiddenRetryActive = false;
    m_hiddenRetryShouldPersist = false;
    m_hiddenRetryCredential.clear();
    m_currentCredential = nullptr;
    m_scanPending = false;
    m_manualSwitchPending = false;
    m_manualSwitchResultReady = false;
    setWiFiState(WiFiState::IDLE);
}
void MyNetworkManager::printStatus(Stream& output) { output.println("Net Status OK"); m_credentialStore.printStatus(output); }
void MyNetworkManager::startScan() {
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.disconnect(false, true);

    lcd_ref.message(0, 1, "Scanning WiFi...", true);
    m_scanPending = false;
    m_currentCredential = nullptr;
    m_stateStartTime = millis();
    m_credentialStore.resetConnectionAttempt();

    const int scanStart = WiFi.scanNetworks(true, true);
    if (scanStart == WIFI_SCAN_FAILED) {
        Serial.println("[WIFI] Failed to start async scan.");
        m_credentialStore.noteScanFailure(WIFI_SCAN_FAILED);
        m_lastScanCompletedMs = millis();
        m_credentialStore.resetConnectionAttempt();
        tryNextCredential();
        return;
    }

    setWiFiState(WiFiState::SCANNING);
}
void MyNetworkManager::processScanResults() {
    const int scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_RUNNING) {
        if (millis() - m_stateStartTime > CONNECT_TIMEOUT) {
            Serial.println("[WIFI] Async scan timed out.");
            WiFi.scanDelete();
            m_credentialStore.noteScanFailure(-1);
            m_lastScanCompletedMs = millis();
            m_credentialStore.resetConnectionAttempt();
            tryNextCredential();
        }
        return;
    }

    if (scanResult >= 0) {
        m_credentialStore.updateFromScan(scanResult);
    } else {
        m_credentialStore.noteScanFailure(scanResult);
        Serial.printf("[WIFI] Scan complete returned %d, falling back to stored credentials.\n", scanResult);
    }
    m_lastScanCompletedMs = millis();
    WiFi.scanDelete();
    m_credentialStore.resetConnectionAttempt();
    tryNextCredential();
}
void MyNetworkManager::tryNextCredential() {
    const WifiCredential* cred = m_credentialStore.getNextCredential();
    if (cred) {
        startConnectionAttempt(cred);
        return;
    }

    m_currentCredential = nullptr;
    m_connectionFailures++;
    if (gprs_enabled && m_connectionFailures >= MAX_FAILURES_BEFORE_GPRS) {
        m_stateStartTime = 0;
        m_retryDelay = MIN_RETRY_DELAY;
        setWiFiState(WiFiState::FALLBACK_GPRS_WAIT);
        return;
    }

    m_stateStartTime = millis();
    unsigned long nextDelay = m_retryDelay;
    if (nextDelay < MIN_RETRY_DELAY)
        nextDelay = MIN_RETRY_DELAY;
    nextDelay *= 2UL;
    if (nextDelay > MAX_RETRY_DELAY)
        nextDelay = MAX_RETRY_DELAY;
    m_retryDelay = nextDelay;
    setWiFiState(WiFiState::RETRY_DELAY);
}
void MyNetworkManager::startConnectionAttempt(const WifiCredential* cred) {
    if (!cred) {
        tryNextCredential();
        return;
    }

    const bool hiddenRetryAttempt =
        m_hiddenRetryActive &&
        (cred == &m_hiddenRetryCredential ||
         (m_manualSwitchPending && cred == &m_manualCredential && cred->isHidden));
    if (!hiddenRetryAttempt) {
        m_hiddenRetryActive = false;
        m_hiddenRetryShouldPersist = false;
        m_hiddenRetryCredential.clear();
    }

    m_currentCredential = cred;
    lcd_ref.message(0, 1, "Connecting WiFi...", true);
    lcd_ref.message(0, 2, cred->ssid, true);

    WiFi.disconnect(false, true);
    WiFi.setScanMethod(cred->isHidden ? WIFI_ALL_CHANNEL_SCAN : WIFI_FAST_SCAN);
    const char* passPtr = cred->password[0] != '\0' ? cred->password : nullptr;
    WiFi.begin(cred->ssid, passPtr);

    m_stateStartTime = millis();
    Serial.printf("[WIFI] Async connect start '%s' hidden=%s available=%s\n",
                  cred->ssid,
                  cred->isHidden ? "YES" : "NO",
                  cred->isAvailable ? "YES" : "NO");
    setWiFiState(WiFiState::CONNECTING);
}
void MyNetworkManager::handleConnecting() {
    if (!m_currentCredential) {
        m_stateStartTime = millis();
        setWiFiState(WiFiState::RETRY_DELAY);
        return;
    }

    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected to '%s' IP=%s\n",
                      WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str());
        if (m_manualSwitchPending) {
            if (WiFi.SSID() == String(m_manualCredential.ssid)) {
                finishManualWiFiConnect(true);
                return;
            }
            Serial.printf("[WIFI] Manual switch reached unexpected SSID '%s' while targeting '%s'\n",
                          WiFi.SSID().c_str(),
                          m_manualCredential.ssid);
            WiFi.disconnect(false, true);
            m_currentCredential = nullptr;
            if (m_manualSwitchAttempt >= m_manualSwitchMaxAttempts) {
                finishManualWiFiConnect(false);
            } else {
                m_stateStartTime = millis();
                setWiFiState(WiFiState::RETRY_DELAY);
            }
            return;
        }
        if (m_hiddenRetryShouldPersist && m_currentCredential && !m_currentCredential->isBuiltIn) {
            m_credentialStore.addCredential(m_currentCredential->ssid,
                                            m_currentCredential->password,
                                            true);
        }
        m_hiddenRetryActive = false;
        m_hiddenRetryShouldPersist = false;
        m_hiddenRetryCredential.clear();
        m_currentCredential = nullptr;
        m_connectionFailures = 0;
        m_retryDelay = MIN_RETRY_DELAY;
        markGprsSessionDown();
        pinMode(MODEM_POWER_ON, OUTPUT);
        digitalWrite(MODEM_POWER_ON, LOW);
        setWiFiState(WiFiState::CONNECTED);
        return;
    }

    const unsigned long timeoutMs = m_currentCredential->isHidden ? (CONNECT_TIMEOUT + 10000UL) : CONNECT_TIMEOUT;
    const bool timedOut = (millis() - m_stateStartTime) >= timeoutMs;
    const bool hardFail = (st == WL_CONNECT_FAILED) || (!m_currentCredential->isHidden && st == WL_NO_SSID_AVAIL);
    if (!timedOut && !hardFail)
        return;

    const bool shouldRetryAsHidden =
        !m_hiddenRetryActive &&
        !m_currentCredential->isHidden &&
        !m_currentCredential->isBuiltIn &&
        !m_currentCredential->isAvailable &&
        (st == WL_NO_SSID_AVAIL || timedOut);

    if (shouldRetryAsHidden) {
        Serial.printf("[WIFI] Retry '%s' as hidden SSID after status=%d (%s)\n",
                      m_currentCredential->ssid,
                      static_cast<int>(st),
                      wlStatusName(st));
        WiFi.disconnect(false, true);
        m_hiddenRetryActive = true;
        if (m_manualSwitchPending) {
            m_manualCredential.isHidden = true;
            if (m_manualSwitchMaxAttempts < 3)
                m_manualSwitchMaxAttempts = 3;
            m_currentCredential = nullptr;
            startConnectionAttempt(&m_manualCredential);
            return;
        }

        m_hiddenRetryCredential = *m_currentCredential;
        m_hiddenRetryCredential.isHidden = true;
        m_hiddenRetryShouldPersist = true;
        m_currentCredential = nullptr;
        startConnectionAttempt(&m_hiddenRetryCredential);
        return;
    }

    Serial.printf("[WIFI] Connect failed '%s' status=%d (%s)\n",
                  m_currentCredential->ssid,
                  static_cast<int>(st),
                  wlStatusName(st));
    WiFi.disconnect(false, true);
    m_currentCredential = nullptr;
    if (m_manualSwitchPending) {
        if (m_manualSwitchAttempt >= m_manualSwitchMaxAttempts) {
            finishManualWiFiConnect(false);
        } else {
            m_hiddenRetryActive = false;
            m_hiddenRetryShouldPersist = false;
            m_hiddenRetryCredential.clear();
            m_stateStartTime = millis();
            setWiFiState(WiFiState::RETRY_DELAY);
        }
        return;
    }
    m_hiddenRetryActive = false;
    m_hiddenRetryShouldPersist = false;
    m_hiddenRetryCredential.clear();
    tryNextCredential();
}
void MyNetworkManager::handleRetryDelay() {
    if (m_scanPending) {
        startScan();
        return;
    }
    if ((millis() - m_stateStartTime) >= m_retryDelay) {
        startScan();
    }
}
void MyNetworkManager::setWiFiState(WiFiState newState) {
    if (m_wifiState == newState)
        return;
    Serial.printf("[WIFI] State %s -> %s\n", wifiStateName(m_wifiState), wifiStateName(newState));
    m_wifiState = newState;
}

void MyNetworkManager::resetGprsSetupState() {
    m_gprsSetupStage = GprsSetupStage::IDLE;
    m_gprsStageDeadlineMs = 0;
    m_gprsLocalIp[0] = '\0';
}

void MyNetworkManager::markGprsSessionDown() {
    m_gprsSessionUp = false;
    m_gprsNetworkAttached = false;
    m_lastGprsVerifiedUpMs = 0;
    m_lastGprsHealthPollMs = 0;
    m_lastGprsSignalPollMs = 0;
    m_lastGprsSignalQuality = 0;
    resetGprsSetupState();
    m_gprsLocalIp[0] = '\0';
}

void MyNetworkManager::markGprsSessionUp(const char* localIp) {
    m_gprsSessionUp = true;
    m_gprsNetworkAttached = true;
    m_lastGprsVerifiedUpMs = millis();
    m_lastGprsHealthPollMs = millis();
    if (localIp && localIp[0] != '\0') {
        strncpy(m_gprsLocalIp, localIp, sizeof(m_gprsLocalIp) - 1);
        m_gprsLocalIp[sizeof(m_gprsLocalIp) - 1] = '\0';
    } else if (m_gprsLocalIp[0] == '\0') {
        strncpy(m_gprsLocalIp, "unknown", sizeof(m_gprsLocalIp) - 1);
        m_gprsLocalIp[sizeof(m_gprsLocalIp) - 1] = '\0';
    }
}

bool MyNetworkManager::hasBlockingBudget(uint16_t controlBudgetMs, uint16_t requiredMs) const {
    return controlBudgetMs >= requiredMs;
}

bool MyNetworkManager::runModemAtCommand(const String& command, uint32_t timeoutMs, String* response) {
    while (SerialAT.available() > 0) {
        SerialAT.read();
    }

    SerialAT.print("AT");
    SerialAT.print(command);
    SerialAT.print("\r\n");
    SerialAT.flush();
    esp_task_wdt_reset();

    String localResponse;
    const int8_t rc = modem.waitResponse(timeoutMs, localResponse);
    if (response) {
        *response = localResponse;
    }
    return rc == 1;
}

bool MyNetworkManager::isSimReadyQuick() {
    String response;
    if (!runModemAtCommand("+CPIN?", kGprsShortCommandTimeoutMs, &response))
        return false;
    return response.indexOf("READY") >= 0;
}

bool MyNetworkManager::isNetworkRegisteredQuick() {
    String response;
    if (!runModemAtCommand("+CREG?", kGprsShortCommandTimeoutMs, &response))
        return false;

    const int commaPos = response.lastIndexOf(',');
    if (commaPos < 0 || (commaPos + 1) >= static_cast<int>(response.length()))
        return false;

    const int stat = response.substring(commaPos + 1).toInt();
    return stat == 1 || stat == 5;
}

bool MyNetworkManager::queryBearerStatusQuick(char* outIp, size_t outLen) {
    String response;
    if (!runModemAtCommand("+SAPBR=2,1", kGprsMediumCommandTimeoutMs, &response))
        return false;
    if (response.indexOf("+SAPBR: 1,1") < 0)
        return false;
    return extractIPv4FromText(response, outIp, outLen);
}

bool MyNetworkManager::queryLocalIpQuick(char* outIp, size_t outLen) {
    String response;
    if (!runModemAtCommand("+CIFSR;E0", kGprsMediumCommandTimeoutMs, &response))
        return false;
    return extractIPv4FromText(response, outIp, outLen);
}

bool MyNetworkManager::readSignalQualityQuick(int& outSignal) {
    String response;
    if (!runModemAtCommand("+CSQ", kGprsShortCommandTimeoutMs, &response))
        return false;

    const char* raw = response.c_str();
    const char* marker = strstr(raw, "+CSQ:");
    if (!marker)
        return false;

    int rssi = 99;
    int ber = 99;
    if (sscanf(marker, "+CSQ: %d,%d", &rssi, &ber) != 2)
        return false;

    outSignal = (rssi == 99) ? 0 : rssi;
    return true;
}

bool MyNetworkManager::isGprsFallbackState(WiFiState state) const {
    switch (state) {
    case WiFiState::FALLBACK_GPRS_WAIT:
    case WiFiState::FALLBACK_GPRS_MODEM_INIT:
    case WiFiState::FALLBACK_GPRS_SIM_READY:
    case WiFiState::FALLBACK_GPRS_NETWORK_ATTACH:
    case WiFiState::FALLBACK_GPRS_CONNECTING:
        return true;
    default:
        return false;
    }
}

void MyNetworkManager::scheduleNextGprsFallbackAttempt() {
    markGprsSessionDown();
    m_scanPending = true;
    m_currentCredential = nullptr;
    m_connectionFailures = MAX_FAILURES_BEFORE_GPRS;
    unsigned long nextDelay = m_retryDelay * 2UL;
    if (nextDelay < MIN_RETRY_DELAY)
        nextDelay = MIN_RETRY_DELAY;
    if (nextDelay > MAX_RETRY_DELAY)
        nextDelay = MAX_RETRY_DELAY;
    m_retryDelay = nextDelay;
    m_stateStartTime = millis();
    setWiFiState(WiFiState::FALLBACK_GPRS_WAIT);
}

void MyNetworkManager::handleGprsFallback(uint16_t controlBudgetMs) {
    if (!gprs_enabled) {
        markGprsSessionDown();
        m_stateStartTime = millis();
        m_retryDelay = MIN_RETRY_DELAY;
        setWiFiState(WiFiState::RETRY_DELAY);
        return;
    }

    if (m_gprsSessionUp && m_gprsNetworkAttached) {
        const unsigned long nowMs = millis();
        if ((nowMs - m_lastGprsHealthPollMs) >= kGprsHealthPollIntervalMs) {
            if (hasBlockingBudget(controlBudgetMs, kGprsMediumCommandTimeoutMs)) {
                m_lastGprsHealthPollMs = nowMs;
                char bearerIp[20] = {0};
                if (!queryBearerStatusQuick(bearerIp, sizeof(bearerIp))) {
                    Serial.println("[GPRS] Bearer unhealthy, scheduling reconnect.");
                    scheduleNextGprsFallbackAttempt();
                    return;
                }
                m_lastGprsVerifiedUpMs = nowMs;
                copyText(m_gprsLocalIp, sizeof(m_gprsLocalIp), bearerIp);
            } else if (hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs)) {
                m_lastGprsHealthPollMs = nowMs;
                if (!isNetworkRegisteredQuick()) {
                    Serial.println("[GPRS] Registration lost, scheduling reconnect.");
                    scheduleNextGprsFallbackAttempt();
                    return;
                }
                m_lastGprsVerifiedUpMs = nowMs;
            }
        }

        if ((nowMs - m_lastGprsSignalPollMs) >= kGprsSignalPollIntervalMs &&
            hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs)) {
            int signal = 0;
            m_lastGprsSignalPollMs = nowMs;
            if (readSignalQualityQuick(signal)) {
                m_lastGprsSignalQuality = signal;
            }
        }
        if (m_wifiState != WiFiState::FALLBACK_GPRS_WAIT) {
            m_stateStartTime = nowMs;
            setWiFiState(WiFiState::FALLBACK_GPRS_WAIT);
        }
        return;
    }

    switch (m_wifiState) {
    case WiFiState::FALLBACK_GPRS_WAIT:
        if (m_stateStartTime != 0 && (millis() - m_stateStartTime) < m_retryDelay)
            return;
        if (!hasBlockingBudget(controlBudgetMs, kGprsProbeAtTimeoutMs))
            return;
        m_stateStartTime = millis();
        resetGprsSetupState();
        lcd_ref.message(0, 1, "Fallback GPRS...", true);
        setWiFiState(WiFiState::FALLBACK_GPRS_MODEM_INIT);
        return;

    case WiFiState::FALLBACK_GPRS_MODEM_INIT:
        if (!hasBlockingBudget(controlBudgetMs, kGprsProbeAtTimeoutMs))
            return;
        pinMode(MODEM_POWER_ON, OUTPUT);
        digitalWrite(MODEM_POWER_ON, HIGH);
        SerialAT.begin(115200, SERIAL_8N1, GSM_RX, GSM_TX);
        if (m_gprsSetupStage == GprsSetupStage::IDLE) {
            m_gprsSetupStage = GprsSetupStage::PROBE_AT;
            m_gprsStageDeadlineMs = millis() + kGprsAtProbeWindowMs;
        }

        if (m_gprsSetupStage == GprsSetupStage::PROBE_AT) {
            if (modem.testAT(kGprsProbeAtTimeoutMs)) {
                m_gprsSetupStage = GprsSetupStage::DISABLE_ECHO;
            } else if (millis() >= m_gprsStageDeadlineMs) {
                scheduleNextGprsFallbackAttempt();
            }
            return;
        }

        if (m_gprsSetupStage == GprsSetupStage::DISABLE_ECHO) {
            if (!hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs))
                return;
            if (!runModemAtCommand("E0", kGprsShortCommandTimeoutMs)) {
                scheduleNextGprsFallbackAttempt();
                return;
            }
            m_gprsSetupStage = GprsSetupStage::ENABLE_LOCAL_TIME;
        }

        if (m_gprsSetupStage == GprsSetupStage::ENABLE_LOCAL_TIME) {
            if (!hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs))
                return;
            if (!runModemAtCommand("+CLTS=1", kGprsShortCommandTimeoutMs)) {
                scheduleNextGprsFallbackAttempt();
                return;
            }
            m_gprsSetupStage = GprsSetupStage::IDLE;
            m_gprsStageDeadlineMs = millis() + GPRS_SIM_STATUS_TIMEOUT_MS;
            m_stateStartTime = millis();
            setWiFiState(WiFiState::FALLBACK_GPRS_SIM_READY);
        }
        return;

    case WiFiState::FALLBACK_GPRS_SIM_READY:
        if (!hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs))
            return;
        if (isSimReadyQuick()) {
            m_gprsStageDeadlineMs = millis() + GPRS_NETWORK_ATTACH_TIMEOUT_MS;
            m_stateStartTime = millis();
            setWiFiState(WiFiState::FALLBACK_GPRS_NETWORK_ATTACH);
        } else if (m_gprsStageDeadlineMs != 0 && millis() >= m_gprsStageDeadlineMs) {
            scheduleNextGprsFallbackAttempt();
        } 
        return;

    case WiFiState::FALLBACK_GPRS_NETWORK_ATTACH:
        if (!hasBlockingBudget(controlBudgetMs, kGprsShortCommandTimeoutMs))
            return;
        if (isNetworkRegisteredQuick()) {
            m_gprsNetworkAttached = true;
            m_gprsSetupStage = GprsSetupStage::CONFIGURE_CONTYPE;
            m_gprsStageDeadlineMs = millis() + kGprsSetupAttemptWindowMs;
            m_stateStartTime = millis();
            setWiFiState(WiFiState::FALLBACK_GPRS_CONNECTING);
        } else if (m_gprsStageDeadlineMs != 0 && millis() >= m_gprsStageDeadlineMs) {
            scheduleNextGprsFallbackAttempt();
        }
        return;

    case WiFiState::FALLBACK_GPRS_CONNECTING:
    {
        if (m_gprsStageDeadlineMs != 0 && millis() >= m_gprsStageDeadlineMs) {
            scheduleNextGprsFallbackAttempt();
            return;
        }

        auto runStep = [&](GprsSetupStage stage,
                           uint16_t timeoutMs,
                           const String& command,
                           GprsSetupStage nextStage) -> bool {
            if (m_gprsSetupStage != stage)
                return false;
            if (!hasBlockingBudget(controlBudgetMs, timeoutMs))
                return true;
            if (!runModemAtCommand(command, timeoutMs)) {
                scheduleNextGprsFallbackAttempt();
                return true;
            }
            m_gprsSetupStage = nextStage;
            return true;
        };

        if (runStep(GprsSetupStage::CONFIGURE_CONTYPE, kGprsShortCommandTimeoutMs,
                    String("+SAPBR=3,1,\"Contype\",\"GPRS\""), GprsSetupStage::CONFIGURE_APN)) return;
        if (runStep(GprsSetupStage::CONFIGURE_APN, kGprsShortCommandTimeoutMs,
                    String("+SAPBR=3,1,\"APN\",\"") + cfg_gprs_apn + "\"",
                    cfg_gprs_user.length() > 0 ? GprsSetupStage::CONFIGURE_USER :
                    (cfg_gprs_pass.length() > 0 ? GprsSetupStage::CONFIGURE_PASSWORD : GprsSetupStage::CONFIGURE_PDP))) return;
        if (runStep(GprsSetupStage::CONFIGURE_USER, kGprsShortCommandTimeoutMs,
                    String("+SAPBR=3,1,\"USER\",\"") + cfg_gprs_user + "\"",
                    cfg_gprs_pass.length() > 0 ? GprsSetupStage::CONFIGURE_PASSWORD : GprsSetupStage::CONFIGURE_PDP)) return;
        if (runStep(GprsSetupStage::CONFIGURE_PASSWORD, kGprsShortCommandTimeoutMs,
                    String("+SAPBR=3,1,\"PWD\",\"") + cfg_gprs_pass + "\"",
                    GprsSetupStage::CONFIGURE_PDP)) return;
        if (runStep(GprsSetupStage::CONFIGURE_PDP, kGprsShortCommandTimeoutMs,
                    String("+CGDCONT=1,\"IP\",\"") + cfg_gprs_apn + "\"",
                    GprsSetupStage::ACTIVATE_PDP)) return;
        if (runStep(GprsSetupStage::ACTIVATE_PDP, kGprsLongCommandTimeoutMs,
                    String("+CGACT=1,1"), GprsSetupStage::OPEN_BEARER)) return;
        if (runStep(GprsSetupStage::OPEN_BEARER, kGprsLongCommandTimeoutMs,
                    String("+SAPBR=1,1"), GprsSetupStage::CHECK_BEARER)) return;

        if (m_gprsSetupStage == GprsSetupStage::CHECK_BEARER) {
            if (!hasBlockingBudget(controlBudgetMs, kGprsMediumCommandTimeoutMs))
                return;
            String response;
            if (!runModemAtCommand("+SAPBR=2,1", kGprsMediumCommandTimeoutMs, &response) ||
                response.indexOf("+SAPBR: 1,1") < 0) {
                scheduleNextGprsFallbackAttempt();
                return;
            }
            m_gprsSetupStage = GprsSetupStage::ATTACH_GPRS;
            return;
        }

        if (runStep(GprsSetupStage::ATTACH_GPRS, kGprsMediumCommandTimeoutMs,
                    String("+CGATT=1"), GprsSetupStage::CONFIGURE_CIPMUX)) return;
        if (runStep(GprsSetupStage::CONFIGURE_CIPMUX, kGprsShortCommandTimeoutMs,
                    String("+CIPMUX=1"), GprsSetupStage::CONFIGURE_QUICKSEND)) return;
        if (runStep(GprsSetupStage::CONFIGURE_QUICKSEND, kGprsShortCommandTimeoutMs,
                    String("+CIPQSEND=1"), GprsSetupStage::CONFIGURE_RXGET)) return;
        if (runStep(GprsSetupStage::CONFIGURE_RXGET, kGprsShortCommandTimeoutMs,
                    String("+CIPRXGET=1"), GprsSetupStage::CONFIGURE_CSTT)) return;
        if (runStep(GprsSetupStage::CONFIGURE_CSTT, kGprsMediumCommandTimeoutMs,
                    String("+CSTT=\"") + cfg_gprs_apn + "\",\"" + cfg_gprs_user + "\",\"" + cfg_gprs_pass + "\"",
                    GprsSetupStage::BRING_UP_WIRELESS)) return;
        if (runStep(GprsSetupStage::BRING_UP_WIRELESS, kGprsLongCommandTimeoutMs,
                    String("+CIICR"), GprsSetupStage::QUERY_LOCAL_IP)) return;

        if (m_gprsSetupStage == GprsSetupStage::QUERY_LOCAL_IP) {
            if (!hasBlockingBudget(controlBudgetMs, kGprsMediumCommandTimeoutMs))
                return;
            char localIp[20] = {0};
            if (!queryLocalIpQuick(localIp, sizeof(localIp))) {
                scheduleNextGprsFallbackAttempt();
                return;
            }
            copyText(m_gprsLocalIp, sizeof(m_gprsLocalIp), localIp);
            m_gprsSetupStage = GprsSetupStage::CONFIGURE_DNS;
            return;
        }

        if (runStep(GprsSetupStage::CONFIGURE_DNS, kGprsShortCommandTimeoutMs,
                    String("+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\""), GprsSetupStage::COMPLETE)) return;

        if (m_gprsSetupStage == GprsSetupStage::COMPLETE) {
            markGprsSessionUp(m_gprsLocalIp[0] != '\0' ? m_gprsLocalIp : nullptr);
            lcd_ref.message(0, 2, "GPRS Connected", true);
            m_connectionFailures = 0;
            m_retryDelay = MIN_RETRY_DELAY;
            m_stateStartTime = millis();
            setWiFiState(WiFiState::FALLBACK_GPRS_WAIT);
        }
        return;
    }

    default:
        return;
    }
}
void MyNetworkManager::captureManualScanResult(int scanResult) {
    m_manualSwitchResult.scanCode = scanResult;
    m_manualSwitchResult.scanSeen = false;
    m_manualSwitchResult.scanRssi = -127;
    m_manualSwitchResult.scanOpenNetwork = false;
    m_manualCredential.isAvailable = false;
    m_manualCredential.lastRssi = -100;

    if (scanResult < 0)
        return;

    for (int i = 0; i < scanResult; i++) {
        if (WiFi.SSID(i) != String(m_manualCredential.ssid))
            continue;
        m_manualSwitchResult.scanSeen = true;
        m_manualSwitchResult.scanRssi = WiFi.RSSI(i);
        m_manualSwitchResult.scanOpenNetwork = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        m_manualCredential.isAvailable = true;
        m_manualCredential.lastRssi = WiFi.RSSI(i);
        break;
    }
}
void MyNetworkManager::finishManualWiFiConnect(bool success) {
    m_manualSwitchResult.success = success;
    m_manualSwitchResult.connectedSsid = isWiFiConnected() ? WiFi.SSID() : "";
    m_manualSwitchResult.wifiStatus = static_cast<int>(WiFi.status());
    m_manualSwitchResult.forceHidden = m_manualCredential.isHidden;

    if (success) {
        if (m_manualSwitchSaveCredential) {
            m_credentialStore.addCredential(m_manualCredential.ssid,
                                            m_manualCredential.password,
                                            m_manualCredential.isHidden);
        }
        m_connectionFailures = 0;
        m_retryDelay = MIN_RETRY_DELAY;
        pinMode(MODEM_POWER_ON, OUTPUT);
        digitalWrite(MODEM_POWER_ON, LOW);
        setWiFiState(WiFiState::CONNECTED);
    } else {
        // Jangan langsung lompat ke auto-scan di iterasi yang sama agar hasil manual switch
        // tetap deterministik di sisi operator. Auto recovery tetap jalan setelah cooldown singkat.
        m_scanPending = false;
        m_retryDelay = MIN_RETRY_DELAY;
        m_stateStartTime = millis();
        setWiFiState(WiFiState::RETRY_DELAY);
    }

    m_manualSwitchPending = false;
    m_manualSwitchResultReady = true;
    m_manualSwitchAttempt = 0;
    m_manualSwitchMaxAttempts = 0;
    m_hiddenRetryActive = false;
    m_hiddenRetryShouldPersist = false;
    m_hiddenRetryCredential.clear();
    m_currentCredential = nullptr;
}
void MyNetworkManager::startManualWiFiAttempt() {
    if (!m_manualSwitchPending)
        return;

    if (m_manualSwitchAttempt >= m_manualSwitchMaxAttempts) {
        finishManualWiFiConnect(false);
        return;
    }

    m_manualSwitchAttempt++;
    Serial.printf("[WIFI] Manual async attempt %u/%u to '%s'\n",
                  static_cast<unsigned>(m_manualSwitchAttempt),
                  static_cast<unsigned>(m_manualSwitchMaxAttempts),
                  m_manualCredential.ssid);
    startConnectionAttempt(&m_manualCredential);
}
void MyNetworkManager::_printModemErrorCause() {}

bool MyNetworkManager::fetchCameraStatus(CloudFogSnapshot& outSnapshot, uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    outSnapshot = CloudFogSnapshot{};
    String url = String(CAMERA_STATUS_URL_BASE) + String(GH_ID_CONFIG);
    return _performHttpRequest(url.c_str(), "GET", "CAM_ST", nullptr, [&](JsonDocument& d) {
        JsonObject data = d["data"];
        if (!data.isNull()) {
            if (!parseFogStatus(data["isFoggy"], outSnapshot.foggy))
                return false;
            outSnapshot.valid = true;
            return true;
        }
        return false;
    }, true, timeoutMs);
}
void MyNetworkManager::qosCheck_Camera(Stream& output) {
#if GH_ID_CONFIG == 2
    String url = String(CAMERA_STATUS_URL_BASE) + String(GH_ID_CONFIG);
    runSingleUrlQoS("Camera Status", url, "GET", nullptr, 3, output);
#else
    output.println("Command not available for GH 1");
#endif
}
