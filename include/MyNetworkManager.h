#ifndef MY_NETWORK_MANAGER_H
#define MY_NETWORK_MANAGER_H

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <functional>
#include "SensorDataManager.h"
#include "LCDDisplay.h"
#include "WiFiCredentialStore.h"
#include "root_ca.h"
#include "RelayController.h" 
#include <TinyGsmClient.h>
#include <Stream.h>

struct QoSMetrics {
    float avgLatencyMs;
    float packetLossPercent;
    float throughputKbps;
    float jitterMs;
    String score;
    String targetName;
};

struct SpecificWiFiConnectResult {
    bool success = false;
    String requestedSsid;
    String connectedSsid;
    int wifiStatus = WL_DISCONNECTED;
    bool forceHidden = false;
    bool scanSeen = false;
    int scanRssi = -127;
    int scanCode = -2;
    bool scanOpenNetwork = false;
};

struct HttpTimeInfo {
    uint32_t epoch = 0;
    char timezone[40] = {0};
    char utcOffset[8] = {0};
};

struct ModemTimeInfo {
    uint32_t epoch = 0;
    char utcOffset[8] = {0};
};

struct CloudSensorSnapshot {
    bool valid = false;
    bool controlReady = false;
    bool hasTemp = false;
    bool hasHum = false;
    bool hasLight = false;
    float temp = 0.0f;
    float hum = 0.0f;
    float light = 0.0f;
};

struct CloudThresholdSnapshot {
    bool valid = false;
    bool hasTemp = false;
    bool hasHum = false;
    bool hasLight = false;
    float tempMin = 23.0f;
    float tempMax = 29.0f;
    float humMin = 58.0f;
    float humMax = 78.0f;
    float lightMin = 10750.0f;
    float lightMax = 21500.0f;
};

struct CloudScheduleSnapshot {
    bool valid = false;
    ScheduleConfig schedules[MAX_SCHEDULES] = {};
};

struct CloudFogSnapshot {
    bool valid = false;
    bool foggy = false;
};

class MyNetworkManager {
public:
    enum class GprsSetupStage : uint8_t {
        IDLE = 0,
        PROBE_AT,
        DISABLE_ECHO,
        ENABLE_LOCAL_TIME,
        CONFIGURE_CONTYPE,
        CONFIGURE_APN,
        CONFIGURE_USER,
        CONFIGURE_PASSWORD,
        CONFIGURE_PDP,
        ACTIVATE_PDP,
        OPEN_BEARER,
        CHECK_BEARER,
        ATTACH_GPRS,
        CONFIGURE_CIPMUX,
        CONFIGURE_QUICKSEND,
        CONFIGURE_RXGET,
        CONFIGURE_CSTT,
        BRING_UP_WIRELESS,
        QUERY_LOCAL_IP,
        CONFIGURE_DNS,
        COMPLETE
    };

    enum class WiFiState {
        IDLE,
        SCANNING,
        CONNECTING,
        CONNECTED,
        RETRY_DELAY,
        FALLBACK_GPRS_WAIT,
        FALLBACK_GPRS_MODEM_INIT,
        FALLBACK_GPRS_SIM_READY,
        FALLBACK_GPRS_NETWORK_ATTACH,
        FALLBACK_GPRS_CONNECTING
    };

    enum class UplinkMode : uint8_t {
        AUTO = 0,
        DIRECT = 1,
        RELAY = 2
    };
    
    MyNetworkManager(SensorDataManager& data, LCDDisplay& display, RelayController& relayCtrl);
    
    void begin(const char* token, const char* ta_token, const char* th_url, const char* nd_url, 
               const char* nd_url_base, const char* post_url, const char* get_url,
               const char* schedule_url, 
               const char* gprs_apn, const char* gprs_user, const char* gprs_pass, 
               const char* sim_pin);
    
    void begin(const char* ssid, const char* pwd, const char* token, const char* ta_token,
               const char* th_url, const char* nd_url, const char* nd_url_base, 
               const char* post_url, const char* get_url, 
               const char* schedule_url, 
               const char* gprs_apn, 
               const char* gprs_user, const char* gprs_pass, const char* sim_pin);
    
    void handleWiFi(uint16_t controlBudgetMs = 0);
    // Queues/maintains the managed WiFi state machine; returns true only if WiFi is already connected.
    bool connectWiFi();
    // Queues an async manual WiFi switch; consumeSpecificWiFiConnectResult() reports the final outcome.
    bool connectSpecificWiFi(const char* ssid, const char* password, bool saveCredential = true, bool assumeHidden = false);
    // Queues an async manual WiFi switch; returns false only if the request could not be queued.
    bool requestSpecificWiFiConnect(const char* ssid, const char* password, bool saveCredential = true, bool assumeHidden = false);
    bool consumeSpecificWiFiConnectResult(SpecificWiFiConnectResult& outResult);
    bool isSpecificWiFiConnectPending() const { return m_manualSwitchPending; }
    String getPendingSpecificWiFiSsid() const { return m_manualSwitchPending ? String(m_manualCredential.ssid) : String(); }
    // Queues/maintains the managed GPRS state machine; returns true only if GPRS is already connected.
    bool connectMobile();
    void triggerRescan();
    void requestUiWiFiScan() { triggerRescan(); }
    
    bool isConnected();
    bool isWiFiConnected();
    bool isGprsConnected();
    int getSignalQuality();
    String getActiveSSID();
    WiFiState getWiFiState() const { return m_wifiState; }
    String getNetworkTimeString();
    unsigned long getLastWiFiScanCompletedMs() const { return m_lastScanCompletedMs; }
    size_t getLastWiFiScanResultCount() const { return m_credentialStore.getLastScanResultCount(); }
    const WiFiScanRecord* getLastWiFiScanResults() const { return m_credentialStore.getLastScanResults(); }
    int getLastWiFiScanCode() const { return m_credentialStore.getLastScanCode(); }
    void setUplinkMode(UplinkMode mode);
    UplinkMode getUplinkMode() const { return m_uplinkMode; }
    void copyUplinkModeString(char* out, size_t out_len) const;
    void copyActiveUplinkRouteString(char* out, size_t out_len) const;
    String resolveUplinkUrl(const String& url) const;
    const String& getWorldTimeUrl() const { return cfg_worldtime_url; }
    
    WiFiCredentialStore& getCredentialStore() { return m_credentialStore; }
    bool addWiFiCredential(const char* ssid, const char* password, bool hidden = false);
    bool removeWiFiCredential(const char* ssid);
    
    bool fetchThresholds(CloudThresholdSnapshot& outSnapshot, uint16_t timeoutMs = 0);
    bool fetchNodeData(CloudSensorSnapshot& outSnapshot, uint16_t timeoutMs = 0);
    bool fetchSchedules(CloudScheduleSnapshot& outSnapshot, uint16_t timeoutMs = 0); 
    bool fetchHttpTimeInfo(HttpTimeInfo& outInfo, uint16_t timeoutMs = 0);
    bool fetchModemTimeInfo(ModemTimeInfo& outInfo);
    uint32_t fetchHttpTimeEpoch(uint16_t timeoutMs = 0);
    bool postSingleDeviceStatus(int ghId, const char* key, bool status, uint16_t timeoutMs = 0);
    bool getDeviceStatus(int ghId, bool& exh, bool& deh, bool& blw, uint16_t timeoutMs = 0);
    bool fetchBundleForGreenhouse(int greenhouseId,
                                  CloudSensorSnapshot& sensorOut,
                                  CloudThresholdSnapshot& thresholdOut,
                                  CloudFogSnapshot* fogOut,
                                  Stream& output,
                                  uint16_t timeoutMs = 0);
    void disconnectAll();
    
    // [FIX] Deklarasi hanya SATU KALI di sini
    bool fetchCameraStatus(CloudFogSnapshot& outSnapshot, uint16_t timeoutMs = 0); 
    void qosCheck_Camera(Stream& output);

    QoSMetrics runSingleUrlQoS(String targetName, String url, const char* method, const char* payload, int samples, Stream& output);
    void qosCheck_Threshold(Stream& output);
    void qosCheck_NodeData(Stream& output);
    void qosCheck_DeviceStatusGet(Stream& output);
    void qosCheck_DeviceStatusPost(int ghId, Stream& output);
    void qosCheck_Firmware(Stream& output);
    void printStatus(Stream& output);
    void updateApiConfig(const char* token,
                         const char* ta_token,
                         const char* th_url,
                         const char* nd_url,
                         const char* nd_url_base,
                         const char* post_url,
                         const char* get_url,
                         const char* schedule_url);

private:
    String cfg_ssid, cfg_pwd;
    String cfg_gprs_apn, cfg_gprs_user, cfg_gprs_pass, cfg_sim_pin;
    String cfg_th_url, cfg_nd_url, cfg_nd_url_base, cfg_auth_token, cfg_ta_token, cfg_worldtime_url;
    String cfg_post_url, cfg_get_url;
    String cfg_schedule_url; 
    
    SensorDataManager& sensorData;
    LCDDisplay& lcd_ref;
    RelayController& relayController; 
    
    WiFiCredentialStore m_credentialStore;
    WiFiState m_wifiState = WiFiState::IDLE;
    const WifiCredential* m_currentCredential = nullptr;
    WifiCredential m_manualCredential;
    WifiCredential m_hiddenRetryCredential;
    unsigned long m_stateStartTime = 0;
    unsigned long m_retryDelay = 5000;
    uint8_t m_connectionFailures = 0;
    bool m_scanPending = false;
    unsigned long m_lastScanCompletedMs = 0;
    bool m_manualSwitchPending = false;
    bool m_manualSwitchSaveCredential = false;
    bool m_manualSwitchResultReady = false;
    uint8_t m_manualSwitchAttempt = 0;
    uint8_t m_manualSwitchMaxAttempts = 0;
    bool m_hiddenRetryActive = false;
    bool m_hiddenRetryShouldPersist = false;
    SpecificWiFiConnectResult m_manualSwitchResult;
    UplinkMode m_uplinkMode = UplinkMode::AUTO;
    bool m_forceRelayNextCloudAttempt = false;
    unsigned long m_relayPinnedUntil = 0;
    GprsSetupStage m_gprsSetupStage = GprsSetupStage::IDLE;
    unsigned long m_gprsStageDeadlineMs = 0;
    bool m_gprsSessionUp = false;
    bool m_gprsNetworkAttached = false;
    unsigned long m_lastGprsVerifiedUpMs = 0;
    unsigned long m_lastGprsHealthPollMs = 0;
    unsigned long m_lastGprsSignalPollMs = 0;
    char m_gprsLocalIp[20] = {0};
    int m_lastGprsSignalQuality = 0;
    
    static constexpr unsigned long MIN_RETRY_DELAY = 5000;
    static constexpr unsigned long MAX_RETRY_DELAY = 60000;
    static constexpr unsigned long CONNECT_TIMEOUT = 15000;
    static constexpr uint8_t MAX_FAILURES_BEFORE_GPRS = 6;
    static constexpr unsigned long RELAY_FALLBACK_PIN_MS = 30UL * 60UL * 1000UL;

    TinyGsm modem;
    TinyGsmClient gprsClient;
    TinyGsmClientSecure gprsSecureClient;
    JsonDocument jsonDoc;
    JsonDocument postJsonDoc;

    void startScan();
    void processScanResults();
    void tryNextCredential();
    void startConnectionAttempt(const WifiCredential* cred);
    void handleConnecting();
    void handleRetryDelay();
    void setWiFiState(WiFiState newState);
    void handleManualWiFiConnect();
    void handleGprsFallback(uint16_t controlBudgetMs);
    void captureManualScanResult(int scanResult);
    void finishManualWiFiConnect(bool success);
    void startManualWiFiAttempt();
    bool isGprsFallbackState(WiFiState state) const;
    void scheduleNextGprsFallbackAttempt();
    void resetGprsSetupState();
    void markGprsSessionDown();
    void markGprsSessionUp(const char* localIp);
    bool hasBlockingBudget(uint16_t controlBudgetMs, uint16_t requiredMs) const;
    bool runModemAtCommand(const String& command, uint32_t timeoutMs, String* response = nullptr);
    bool isSimReadyQuick();
    bool isNetworkRegisteredQuick();
    bool queryBearerStatusQuick(char* outIp, size_t outLen);
    bool queryLocalIpQuick(char* outIp, size_t outLen);
    bool readSignalQualityQuick(int& outSignal);
    bool shouldUseRelayForCloudRequest() const;
    bool shouldTreatAsCloudFailure(int httpStatusCode, bool wafBlocked) const;
    bool shouldFallbackToRelay(const String& originalUrl, bool usedRelay, int httpStatusCode, bool wafBlocked) const;
    void activateRelayFallback();
    void clearRelayFallback();
    bool shouldUseTaTokenForUrl(const String& url) const;
    String normalizeOriginUrl(const String& url) const;
    bool isRelayCapableUrl(const String& url) const;
    String buildRelayUrl(const String& url) const;
    
    void _printModemErrorCause();
    bool _performHttpRequestWiFi(const String& url, const char* method, const char* payload, bool needsAuth, bool useTaToken, int& httpStatusCode, bool& wafBlocked, std::function<bool(JsonDocument&)> cb, uint16_t timeoutMs);
    bool _performHttpRequestGPRS(const String& url, const char* method, const char* payload, bool needsAuth, bool useTaToken, int& httpStatusCode, bool& wafBlocked, std::function<bool(JsonDocument&)> cb, uint16_t timeoutMs);
    bool _performHttpRequest(const char* url, const char* method, const char* apiType, const char* payload, std::function<bool(JsonDocument&)> cb, bool needsAuth, uint16_t timeoutMs);
};

#endif // MY_NETWORK_MANAGER_H
