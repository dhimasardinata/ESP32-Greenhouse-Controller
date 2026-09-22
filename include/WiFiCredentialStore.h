///////////////////////////////////////////////////////////////////////////////
// File: WiFiCredentialStore.h
//
// Description: Multi-credential WiFi store for ESP32 gateway.
//              - Built-in Greenhouse-1/Bawah credentials with GH_ID-based priority
//              - Up to 5 user-saved networks with Preferences persistence
//              - RSSI-based sorting and priority selection
///////////////////////////////////////////////////////////////////////////////

#ifndef WIFI_CREDENTIAL_STORE_H
#define WIFI_CREDENTIAL_STORE_H

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <array>

// Maximum saved user networks
constexpr size_t MAX_SAVED_NETWORKS = 5;
constexpr size_t WIFI_SSID_MAX_LEN = 33;
constexpr size_t WIFI_PASS_MAX_LEN = 65;
constexpr size_t MAX_SCAN_CACHE_RESULTS = 12;

struct WiFiScanRecord {
    char ssid[WIFI_SSID_MAX_LEN] = {0};
    int32_t rssi = -100;
    bool secure = false;
    bool valid = false;
};

// Single WiFi credential entry
struct WifiCredential {
    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char password[WIFI_PASS_MAX_LEN] = {0};
    int32_t lastRssi = -100;      // Last seen signal strength
    bool isAvailable = false;     // Currently visible in scan
    bool isBuiltIn = false;       // Greenhouse-1/Bawah (not deletable)
    bool isHidden = false;        // Hidden network (try even if not scanned)
    
    bool isEmpty() const { return ssid[0] == '\0'; }
    void clear() { memset(this, 0, sizeof(*this)); lastRssi = -100; }
};

class WiFiCredentialStore {
public:
    WiFiCredentialStore();
    
    // Lifecycle
    void init();
    
    // User credential management
    bool addCredential(const char* ssid, const char* password, bool hidden = false);
    bool removeCredential(const char* ssid);
    bool hasCredential(const char* ssid) const;
    
    // Update availability from scan results
    void updateFromScan(int networkCount);
    void noteScanFailure(int scanCode);
    
    // Get next credential to try (priority-based)
    const WifiCredential* getNextCredential();
    void resetConnectionAttempt();
    
    // Getters
    size_t getSavedCount() const;
    size_t getTotalAvailableCount() const;
    size_t getLastScanResultCount() const { return m_lastScanResultCount; }
    const WiFiScanRecord* getLastScanResults() const { return m_lastScanResults.data(); }
    int getLastScanCode() const { return m_lastScanCode; }
    const WifiCredential* getPrimaryGH() const { return &m_primaryGH; }
    const WifiCredential* getSecondaryGH() const { return &m_secondaryGH; }
    
    // For WebSerial display
    void printStatus(Stream& output) const;

private:
    void loadFromPreferences();
    void saveToPreferences();
    void setupBuiltInCredentials();
    void sortByRssi();
    void resetAvailability();
    void clearLastScanResults(int scanCode);
    
    // Built-in credentials (based on GH_ID_CONFIG)
    WifiCredential m_primaryGH;
    WifiCredential m_secondaryGH;
    
    // User-saved credentials
    std::array<WifiCredential, MAX_SAVED_NETWORKS> m_savedCredentials;
    std::array<WiFiScanRecord, MAX_SCAN_CACHE_RESULTS> m_lastScanResults;
    size_t m_lastScanResultCount = 0;
    int m_lastScanCode = -2;
    
    // Connection attempt tracking
    uint8_t m_currentAttemptIndex = 0;
    bool m_triedPrimary = false;
    bool m_triedSecondary = false;
    
    // Preferences handle
    Preferences m_prefs;
    static constexpr const char* PREFS_NAMESPACE = "wifi_store";
};

#endif // WIFI_CREDENTIAL_STORE_H
