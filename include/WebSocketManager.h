#ifndef WEBSOCKET_MANAGER_H
#define WEBSOCKET_MANAGER_H

#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <ArduinoJson.h>

class SensorDataManager;
class RelayController;
class MyNetworkManager;
class RTCManager;

class WebSocketManager {
public:
    WebSocketManager(SensorDataManager& data, RelayController& relays, MyNetworkManager& net, RTCManager& rtc);
    void begin(AsyncWebServer* server);
    
    // HAPUS parameter bool isFoggy
    void broadcastStatus(); 
    void sendMutationResultToClient(uint32_t clientId, const char* type, bool success, const String& message, int idx = -1, const char* requestId = nullptr);
    void pumpAsyncEvents();
    
    void cleanupClients();

private:
    void sendJsonToClient(AsyncWebSocketClient* client, JsonDocument& doc);
    void sendCachedStatus(AsyncWebSocketClient* client);
    void sendRecentMutationResults(AsyncWebSocketClient* client);
    void sendWifiScanResult(AsyncWebSocketClient* client);
    void sendCachedWifiScanResult(AsyncWebSocketClient* client);
    void sendWifiLog(AsyncWebSocketClient* client, const char* level, const String& message);
    void handleWifiScanRequest(AsyncWebSocketClient* client);
    void handleWifiChange(AsyncWebSocketClient* client, const char* ssid, const char* password);
    void sendCachedWifiChangeResult(AsyncWebSocketClient* client);
    void handleAdminAuth(AsyncWebSocketClient* client, const char* password);
    bool isAdminRequestAuthorized(AsyncWebSocketClient* client, JsonDocument& doc);
    void sendAdminAuthRequired(AsyncWebSocketClient* client, const char* action);
    static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
    
    SensorDataManager& sensorData_ref;
    RelayController& relay_ref;
    MyNetworkManager& net_ref;
    RTCManager& rtc_ref;
    String cachedStatusPayload;
    String cachedWifiScanPayload;
    unsigned long cachedWifiScanAtMs = 0;
    String cachedWifiChangePayload;
    unsigned long cachedWifiChangeAtMs = 0;
    bool wifiScanAwaitingResult = false;
    bool wifiScanUsesStandaloneScan = false;
    unsigned long pendingWifiScanRequestedAtMs = 0;
    bool wifiChangeAwaitingResult = false;
    bool pendingWifiAutoPasswordApplied = false;
    bool pendingWifiPasswordProvided = false;
    bool pendingWifiForceHidden = false;
    String pendingWifiTargetSsid;

    static AsyncWebSocket* _ws;
    static WebSocketManager* _instance;
};

#endif // WEBSOCKET_MANAGER_H
