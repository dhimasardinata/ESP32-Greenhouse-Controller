///////////////////////////////////////////////////////////////////////////////////
// File: ConfigManager.h
//
// Description: Mendeklarasikan antarmuka publik untuk modul ConfigManager.
//              Menyediakan deklarasi fungsi untuk memuat, menyimpan,
//              mengatur portal, mengatur handler OTA, dan menghapus konfigurasi.
///////////////////////////////////////////////////////////////////////////////////

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <Arduino.h>

// Deklarasi forward class untuk menghindari #include di file header.
// Ini adalah praktik yang baik untuk mempercepat kompilasi dan mengurangi dependensi.
class AsyncWebServer;
class AsyncWebServerRequest;
class LCDDisplay;

// --- Variabel Konfigurasi Global ---
// 'extern' memberitahu kompiler bahwa variabel-variabel ini ada,
// tetapi didefinisikan (dialokasikan memorinya) di file .cpp lain.
extern char active_ssid[64];
extern char active_pwd[64];
extern char active_api_token[128];
extern char active_ta_token[128];
extern char active_th_url[128];
extern char active_nd_url[128];
extern char active_nd_url_base[128];
extern char active_device_status_post_url[128];
extern char active_device_status_get_url[128];
extern char active_admin_pass[64]; // <-- TAMBAHKAN INI
extern bool gprs_enabled;
extern const char OTA_UPDATE_PAGE[] PROGMEM;


// --- Deklarasi Fungsi Publik ---

/**
 * @brief Memuat konfigurasi (SSID, PWD, dll.) dari NVS ke variabel global.
 */
void loadConfiguration();

/**
 * @brief Memulai Access Point dan web server untuk portal konfigurasi awal.
 * Fungsi ini akan memblokir eksekusi hingga selesai atau timeout.
 * @param lcd Referensi ke objek LCD untuk menampilkan status.
 */
void startConfigPortal(LCDDisplay& lcd);

/**
 * @brief Mengatur handler web untuk pembaruan OTA saat perangkat dalam mode operasi normal (STA).
 * @param server Referensi ke objek AsyncWebServer yang sedang berjalan.
 * @param lcd Referensi ke objek LCD untuk menampilkan status.
 */
void setupOtaHandlers(AsyncWebServer& server, LCDDisplay& lcd);

/**
 * @brief Menghapus kredensial WiFi (SSID dan Password) yang tersimpan di NVS.
 * @param lcd Referensi ke objek LCD untuk menampilkan pesan.
 */

void clearWiFiCredentials(LCDDisplay& lcd);

/**
 * @brief Menyimpan konfigurasi GPRS ke NVS
 * @param enabled Status GPRS (true/false)
 */
void saveGPRSConfig(bool enabled);

bool verifyAdminPassword(const String& password);
bool verifyAdminPasswordWithRateLimit(const String& binding,
                                      const String& password,
                                      unsigned long* retryAfterMs = nullptr);
String issueAdminSessionToken(const String& binding = "");
void invalidateAdminSessionToken();
bool isAdminSessionTokenValid(const String& token, const String& binding = "");
String extractAdminTokenFromRequest(AsyncWebServerRequest* request);
String buildAdminRequestBinding(AsyncWebServerRequest* request);
bool isAdminRequestAuthorized(AsyncWebServerRequest* request, bool requireEncryptedProof = false);


#endif // CONFIG_MANAGER_H
