///////////////////////////////////////////////////////////////////////////////
// File: WiFiCredentialStore.cpp
//
// Description: Implementation of multi-credential WiFi store for ESP32.
//              Uses ESP32 Preferences API for persistent storage.
///////////////////////////////////////////////////////////////////////////////

#include "WiFiCredentialStore.h"
#include "config.h"
#include <algorithm>

// Hardcoded GH credentials
namespace {
    const char GH_ATAS_SSID[] = "Greenhouse-1";
    const char GH_BAWAH_SSID[] = "Greenhouse-2";
    const char GH_PASSWORD[] = "change-me-wifi-password";
}

WiFiCredentialStore::WiFiCredentialStore() {
    setupBuiltInCredentials();
}

void WiFiCredentialStore::setupBuiltInCredentials() {
    // GH_ID_CONFIG determines priority:
    // GH 1 (Atas gateway) -> Primary: Greenhouse-1, Secondary: Greenhouse-2
    // GH 2 (Bawah gateway) -> Primary: Greenhouse-2, Secondary: Greenhouse-1
    const char* primarySsid = (GH_ID_CONFIG == 1) ? GH_ATAS_SSID : GH_BAWAH_SSID;
    const char* secondarySsid = (GH_ID_CONFIG == 1) ? GH_BAWAH_SSID : GH_ATAS_SSID;
    
    // Initialize primary
    strncpy(m_primaryGH.ssid, primarySsid, WIFI_SSID_MAX_LEN - 1);
    strncpy(m_primaryGH.password, GH_PASSWORD, WIFI_PASS_MAX_LEN - 1);
    m_primaryGH.isBuiltIn = true;
    
    // Initialize secondary
    strncpy(m_secondaryGH.ssid, secondarySsid, WIFI_SSID_MAX_LEN - 1);
    strncpy(m_secondaryGH.password, GH_PASSWORD, WIFI_PASS_MAX_LEN - 1);
    m_secondaryGH.isBuiltIn = true;
    
    Serial.printf("[WIFI-STORE] GH_ID=%d -> Primary: '%s', Secondary: '%s'\n",
                  GH_ID_CONFIG, primarySsid, secondarySsid);
}

void WiFiCredentialStore::init() {
    loadFromPreferences();
}

void WiFiCredentialStore::loadFromPreferences() {
    if (!m_prefs.begin(PREFS_NAMESPACE, true)) {  // Read-only
        Serial.println("[WIFI-STORE] No saved credentials (namespace not found)");
        return;
    }
    
    size_t count = m_prefs.getUChar("count", 0);
    if (count == 0) {
        m_prefs.end();
        return;
    }
    
    for (size_t i = 0; i < std::min(count, MAX_SAVED_NETWORKS); i++) {
        char keySSID[16], keyPass[16], keyHidden[16];
        snprintf(keySSID, sizeof(keySSID), "ssid%d", i);
        snprintf(keyPass, sizeof(keyPass), "pass%d", i);
        snprintf(keyHidden, sizeof(keyHidden), "hide%d", i);
        
        String ssid = m_prefs.getString(keySSID, "");
        String pass = m_prefs.getString(keyPass, "");
        bool hidden = m_prefs.getBool(keyHidden, false);
        
        if (ssid.length() > 0) {
            strncpy(m_savedCredentials[i].ssid, ssid.c_str(), WIFI_SSID_MAX_LEN - 1);
            strncpy(m_savedCredentials[i].password, pass.c_str(), WIFI_PASS_MAX_LEN - 1);
            m_savedCredentials[i].isHidden = hidden;
            m_savedCredentials[i].isBuiltIn = false;
        }
    }
    
    m_prefs.end();
    Serial.printf("[WIFI-STORE] Loaded %d saved credentials\n", count);
}

void WiFiCredentialStore::saveToPreferences() {
    if (!m_prefs.begin(PREFS_NAMESPACE, false)) {  // Read-write
        Serial.println("[WIFI-STORE] Failed to open preferences for writing");
        return;
    }
    
    m_prefs.clear();
    
    // Count non-empty
    uint8_t count = 0;
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty()) count++;
    }
    
    m_prefs.putUChar("count", count);
    
    size_t idx = 0;
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty()) {
            char keySSID[16], keyPass[16], keyHidden[16];
            snprintf(keySSID, sizeof(keySSID), "ssid%d", idx);
            snprintf(keyPass, sizeof(keyPass), "pass%d", idx);
            snprintf(keyHidden, sizeof(keyHidden), "hide%d", idx);
            
            m_prefs.putString(keySSID, cred.ssid);
            m_prefs.putString(keyPass, cred.password);
            m_prefs.putBool(keyHidden, cred.isHidden);
            idx++;
        }
    }
    
    m_prefs.end();
    Serial.printf("[WIFI-STORE] Saved %d credentials\n", count);
}

bool WiFiCredentialStore::addCredential(const char* ssid, const char* password, bool hidden) {
    if (!ssid || !password) return false;
    
    // Check if already exists - update
    for (auto& cred : m_savedCredentials) {
        if (!cred.isEmpty() && strcmp(cred.ssid, ssid) == 0) {
            strncpy(cred.password, password, WIFI_PASS_MAX_LEN - 1);
            cred.isHidden = hidden;
            saveToPreferences();
            Serial.printf("[WIFI-STORE] Updated credential for '%s'\n", ssid);
            return true;
        }
    }
    
    // Find empty slot
    for (auto& cred : m_savedCredentials) {
        if (cred.isEmpty()) {
            strncpy(cred.ssid, ssid, WIFI_SSID_MAX_LEN - 1);
            strncpy(cred.password, password, WIFI_PASS_MAX_LEN - 1);
            cred.isBuiltIn = false;
            cred.isHidden = hidden;
            saveToPreferences();
            Serial.printf("[WIFI-STORE] Added new credential for '%s'\n", ssid);
            return true;
        }
    }
    
    Serial.println("[WIFI-STORE] No empty slots for new credential");
    return false;
}

bool WiFiCredentialStore::removeCredential(const char* ssid) {
    for (auto& cred : m_savedCredentials) {
        if (!cred.isEmpty() && strcmp(cred.ssid, ssid) == 0) {
            cred.clear();
            saveToPreferences();
            Serial.printf("[WIFI-STORE] Removed credential for '%s'\n", ssid);
            return true;
        }
    }
    return false;
}

bool WiFiCredentialStore::hasCredential(const char* ssid) const {
    // Check built-in
    if (strcmp(ssid, m_primaryGH.ssid) == 0 || strcmp(ssid, m_secondaryGH.ssid) == 0) {
        return true;
    }
    
    // Check saved
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty() && strcmp(cred.ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

void WiFiCredentialStore::clearLastScanResults(int scanCode) {
    for (auto& entry : m_lastScanResults) {
        entry = {};
    }
    m_lastScanResultCount = 0;
    m_lastScanCode = scanCode;
}

void WiFiCredentialStore::noteScanFailure(int scanCode) {
    clearLastScanResults(scanCode);
}

void WiFiCredentialStore::resetAvailability() {
    m_primaryGH.isAvailable = false;
    m_primaryGH.lastRssi = -100;
    m_secondaryGH.isAvailable = false;
    m_secondaryGH.lastRssi = -100;
    
    for (auto& cred : m_savedCredentials) {
        cred.isAvailable = cred.isHidden;  // Hidden assumed available
        cred.lastRssi = cred.isHidden ? -95 : -100;
    }
}

void WiFiCredentialStore::updateFromScan(int networkCount) {
    clearLastScanResults(networkCount);
    resetAvailability();
    
    // Log scanned networks for debugging
    Serial.println("[WIFI-STORE] Scanned networks:");
    for (int i = 0; i < networkCount; i++) {
        String scannedSsid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        Serial.printf("  %d. '%s' (%d dBm)\n", i + 1, scannedSsid.c_str(), rssi);

        if (scannedSsid.length() > 0) {
            size_t cacheIndex = m_lastScanResultCount;
            for (size_t idx = 0; idx < m_lastScanResultCount; ++idx) {
                if (scannedSsid == m_lastScanResults[idx].ssid) {
                    cacheIndex = idx;
                    break;
                }
            }

            const bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            if (cacheIndex < m_lastScanResultCount) {
                if (rssi > m_lastScanResults[cacheIndex].rssi)
                    m_lastScanResults[cacheIndex].rssi = rssi;
                m_lastScanResults[cacheIndex].secure = m_lastScanResults[cacheIndex].secure || secure;
            } else if (m_lastScanResultCount < MAX_SCAN_CACHE_RESULTS) {
                WiFiScanRecord& entry = m_lastScanResults[m_lastScanResultCount++];
                strncpy(entry.ssid, scannedSsid.c_str(), WIFI_SSID_MAX_LEN - 1);
                entry.ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
                entry.rssi = rssi;
                entry.secure = secure;
                entry.valid = true;
            }
        }
        
        // Check built-in
        if (scannedSsid == m_primaryGH.ssid) {
            m_primaryGH.isAvailable = true;
            m_primaryGH.lastRssi = rssi;
        }
        if (scannedSsid == m_secondaryGH.ssid) {
            m_secondaryGH.isAvailable = true;
            m_secondaryGH.lastRssi = rssi;
        }
        
        // Check saved
        for (auto& cred : m_savedCredentials) {
            if (!cred.isEmpty() && scannedSsid == cred.ssid) {
                cred.isAvailable = true;
                cred.lastRssi = rssi;
                Serial.printf("  -> Matched saved: '%s'\n", cred.ssid);
            }
        }
    }
    
    sortByRssi();
    
    Serial.printf("[WIFI-STORE] Primary '%s' %s (%d dBm), Secondary '%s' %s (%d dBm)\n",
                  m_primaryGH.ssid, m_primaryGH.isAvailable ? "OK" : "N/A", m_primaryGH.lastRssi,
                  m_secondaryGH.ssid, m_secondaryGH.isAvailable ? "OK" : "N/A", m_secondaryGH.lastRssi);
    
    // Log saved creds status
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty()) {
            Serial.printf("[WIFI-STORE] Saved '%s' %s (%d dBm)\n", 
                          cred.ssid, cred.isAvailable ? "OK" : "N/A", cred.lastRssi);
        }
    }
}

void WiFiCredentialStore::sortByRssi() {
    std::sort(std::begin(m_savedCredentials), std::end(m_savedCredentials),
        [](const WifiCredential& a, const WifiCredential& b) {
            if (a.isEmpty() != b.isEmpty()) return !a.isEmpty();
            if (a.isAvailable != b.isAvailable) return a.isAvailable;
            return a.lastRssi > b.lastRssi;
        });
}

void WiFiCredentialStore::resetConnectionAttempt() {
    m_currentAttemptIndex = 0;
    m_triedPrimary = false;
    m_triedSecondary = false;
}

const WifiCredential* WiFiCredentialStore::getNextCredential() {
    // Priority 1: Primary GH (if available and untried)
    if (!m_triedPrimary && m_primaryGH.isAvailable) {
        m_triedPrimary = true;
        Serial.printf("[WIFI-STORE] Next: Primary '%s' (RSSI: %d)\n",
                      m_primaryGH.ssid, m_primaryGH.lastRssi);
        return &m_primaryGH;
    }
    
    // Priority 2: Secondary GH (if available and untried)
    if (!m_triedSecondary && m_secondaryGH.isAvailable) {
        m_triedSecondary = true;
        Serial.printf("[WIFI-STORE] Next: Secondary '%s' (RSSI: %d)\n",
                      m_secondaryGH.ssid, m_secondaryGH.lastRssi);
        return &m_secondaryGH;
    }
    
    // Priority 3: User-saved (try ALL non-empty, not just available - scan may miss networks)
    while (m_currentAttemptIndex < MAX_SAVED_NETWORKS) {
        const auto& cred = m_savedCredentials[m_currentAttemptIndex];
        m_currentAttemptIndex++;
        
        if (!cred.isEmpty()) {
            Serial.printf("[WIFI-STORE] Next: User '%s' (RSSI: %d, Available: %s)\n",
                          cred.ssid, cred.lastRssi, cred.isAvailable ? "Yes" : "No");
            return &cred;
        }
    }

    // Priority 4: Fallback built-in GH credentials even if the scan missed them.
    if (!m_triedPrimary) {
        m_triedPrimary = true;
        Serial.printf("[WIFI-STORE] Fallback: Primary '%s' (not seen in scan)\n",
                      m_primaryGH.ssid);
        return &m_primaryGH;
    }

    if (!m_triedSecondary) {
        m_triedSecondary = true;
        Serial.printf("[WIFI-STORE] Fallback: Secondary '%s' (not seen in scan)\n",
                      m_secondaryGH.ssid);
        return &m_secondaryGH;
    }
    
    Serial.println("[WIFI-STORE] No more credentials to try");
    return nullptr;
}

size_t WiFiCredentialStore::getSavedCount() const {
    size_t count = 0;
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty()) count++;
    }
    return count;
}

size_t WiFiCredentialStore::getTotalAvailableCount() const {
    size_t count = 0;
    if (m_primaryGH.isAvailable) count++;
    if (m_secondaryGH.isAvailable) count++;
    for (const auto& cred : m_savedCredentials) {
        if (!cred.isEmpty() && cred.isAvailable) count++;
    }
    return count;
}

void WiFiCredentialStore::printStatus(Stream& output) const {
    output.println("--- WiFi Credentials ---");
    output.printf("Primary: '%s' %s (%d dBm)\n", 
                  m_primaryGH.ssid, 
                  m_primaryGH.isAvailable ? "[Available]" : "[Not Found]",
                  m_primaryGH.lastRssi);
    output.printf("Secondary: '%s' %s (%d dBm)\n", 
                  m_secondaryGH.ssid, 
                  m_secondaryGH.isAvailable ? "[Available]" : "[Not Found]",
                  m_secondaryGH.lastRssi);
    
    output.printf("Saved networks (%d):\n", getSavedCount());
    for (size_t i = 0; i < MAX_SAVED_NETWORKS; i++) {
        const auto& cred = m_savedCredentials[i];
        if (!cred.isEmpty()) {
            output.printf("  %d. '%s' %s (%d dBm)%s\n", 
                          i + 1, cred.ssid,
                          cred.isAvailable ? "[OK]" : "[N/A]",
                          cred.lastRssi,
                          cred.isHidden ? " [Hidden]" : "");
        }
    }
    output.println("------------------------");
}
