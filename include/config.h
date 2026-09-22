///////////////////////////////////////////////////////////////////////////////////
// File: config.h (FINAL VERSION)
//
// Description: File konfigurasi pusat untuk proyek ESP32 T-Call Relay Controller.
//              Berisi semua definisi pin hardware, kredensial default,
//              endpoint API, pengaturan waktu, dan level debug.
///////////////////////////////////////////////////////////////////////////////////

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// REFACTOR: Gunakan enum untuk merepresentasikan indeks relay.
// Ini menghilangkan "magic numbers" (0, 1, 2) dan membuat kode lebih mudah dibaca dan dipelihara.
enum RelayIndex {
    RELAY_EXHAUST = 0,
    RELAY_DEHUMIDIFIER = 1,
    RELAY_BLOWER = 2,
    RELAY_UNUSED = 3
};

// --- Firmware Version ---
// Ubah nomor ini setiap kali Anda membuat perubahan signifikan pada firmware.
#define FIRMWARE_VERSION "3.2.11"

// --- Debug Configuration ---
// Atur level output pada Serial Monitor.
// 0 = Tidak ada output
// 1 = Hanya pesan Error
// 2 = Pesan Error dan Peringatan (Warning)
// 3 = Pesan Info, Error, dan Peringatan (Rekomendasi)
#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 3
#endif

// ==================================================================================
//   Definisi Pin Hardware
// ==================================================================================

// --- Pin SIM800L (TTGO T-Call V1.3/V1.4) ---
const int GSM_TX = 26;
const int GSM_RX = 27;
const int GSM_PWR = 4;         // PWKEY (Power Key)
const int GSM_RST = 5;         // Reset Pin
const int MODEM_POWER_ON = 23;

// --- Pin SD Card (SPI) ---
const int SD_CS = 2;
const int SD_SCK = 18;
const int SD_MISO = 19;
const int SD_MOSI = 13;

// --- Pin Relay (Disesuaikan berdasarkan Greenhouse ID) ---
// GH_ID_CONFIG didefinisikan di file platformio.ini sebagai build flag.
// Ini memungkinkan satu basis kode untuk mengompilasi firmware
// dengan konfigurasi pin yang berbeda untuk setiap perangkat.

#if GH_ID_CONFIG == 1
    #define GH_NAME "GREENHOUSE 1"
    const int RELAY_CH1 = 32; // Terhubung ke RelayIndex::RELAY_EXHAUST
    const int RELAY_CH2 = 33; // Terhubung ke RelayIndex::RELAY_DEHUMIDIFIER
    const int RELAY_CH3 = 14; // Terhubung ke RelayIndex::RELAY_BLOWER
    const int RELAY_CH4 = 12; // Terhubung ke RelayIndex::RELAY_UNUSED

#elif GH_ID_CONFIG == 2
    #define GH_NAME "GREENHOUSE 2"
    const int RELAY_CH1 = 32; // Terhubung ke RelayIndex::RELAY_EXHAUST
    const int RELAY_CH2 = 33; // Terhubung ke RelayIndex::RELAY_DEHUMIDIFIER
    const int RELAY_CH3 = 12; // Terhubung ke RelayIndex::RELAY_BLOWER
    const int RELAY_CH4 = 14; // Terhubung ke RelayIndex::RELAY_UNUSED
    
#else
    #error "GH_ID_CONFIG tidak valid di platformio.ini! Atur ke 1 atau 2."
    #define GH_NAME "INVALID GH"
    const int RELAY_CH1 = -1;
    const int RELAY_CH2 = -1;
    const int RELAY_CH3 = -1;
    const int RELAY_CH4 = -1;
#endif

// --- Pin I2C (untuk LCD & RTC) ---
const int SDA_PIN = 21;
const int SCL_PIN = 22;

// --- Alamat I2C LCD ---
const byte LCD_ADDR = 0x27; // Alamat umum, verifikasi dengan I2C Scanner jika perlu.

// ==================================================================================
//   Kredensial Jaringan & Konfigurasi API (Default)
//   Nilai-nilai ini akan ditimpa oleh konfigurasi yang disimpan dari Web Portal.
// ==================================================================================

// --- WiFi Defaults ---
const char SSID[] PROGMEM = "Greenhouse-WiFi";
const char PWD[] PROGMEM = "change-me-wifi-password";

// --- GPRS Defaults ---
const char GPRS_APN[] PROGMEM = "internet"; // Sesuaikan dengan APN provider SIM Anda
const char GPRS_USER[] PROGMEM = "";
const char GPRS_PASSWORD[] PROGMEM = "";
const char SIM_PIN[] PROGMEM = "";          // Kosongkan jika SIM tidak memiliki PIN

// --- API Endpoint Defaults ---
// CATATAN: URL API utama (TH_URL, ND_URL, dll.) sekarang didefinisikan di
// file platformio.ini sebagai build flag agar spesifik untuk setiap environment.
const char WORLDTIME_URL[] PROGMEM = "https://timeapi.io/api/Time/current/zone?timeZone=Asia/Jakarta";
const char GATEWAY_TIMEZONE_NAME[] PROGMEM = "Asia/Jakarta";
const char GATEWAY_TIMEZONE_POSIX[] PROGMEM = "WIB-7";
const char GATEWAY_TIMEZONE_OFFSET[] PROGMEM = "+07:00";
const char PRIMARY_NTP_SERVER[] PROGMEM = "time.cloudflare.com";
const char SECONDARY_NTP_SERVER[] PROGMEM = "pool.ntp.org";
const char TERTIARY_NTP_SERVER[] PROGMEM = "asia.pool.ntp.org";

// --- API Authentication Default ---
const char AUTH[] PROGMEM = "PASTE_API_TOKEN_HERE";
// Token khusus untuk domain server TA (jadwal & status device)
const char TA_AUTH[] PROGMEM = "PASTE_TA_API_TOKEN_HERE";

// ==================================================================================
//   Konfigurasi Waktu (dalam milidetik, kecuali dinyatakan lain)
// ==================================================================================

const unsigned long LOOP_MS = 5000;                           // Siklus loop utama (kontrol & display)
const unsigned long API_MS = 30 * 1000UL;                     // Interval pengambilan data dari API (30 detik)
const unsigned long CLOUD_CONTROL_DATA_VALID_MS = 60 * 1000UL; // Sensor cloud dianggap sehat untuk kontrol selama 60 detik
const uint16_t HTTP_REQUEST_TIMEOUT_MS = 8000U;               // Batas waktu request kontrol agar loop tidak tertahan terlalu lama
const uint16_t HTTP_CONTROL_SAFE_TIMEOUT_MS = 3000U;          // Timeout request saat berbagi budget dengan control loop
const uint16_t HTTP_CONTROL_MIN_TIMEOUT_MS = 1200U;           // Budget minimum agar request layak dijalankan tanpa mengganggu kontrol
const uint16_t HTTP_CONNECT_TIMEOUT_MS = 5000U;               // Batas waktu connect HTTP sebelum request dibatalkan
const unsigned long CONTROL_LOOP_SAFETY_MARGIN_MS = 750UL;    // Jeda aman yang disisakan sebelum control loop jatuh tempo lagi
const unsigned long TIME_SYNC_INTERVAL = 24 * 3600 * 1000UL;  // Interval sinkronisasi waktu (24 jam)
const unsigned long STALE_DATA_THRESHOLD_MS = 30 * 60 * 1000UL; // Waktu sebelum data dianggap usang (30 menit)
const unsigned long THRESHOLD_STALE_THRESHOLD_MS = 24 * 60 * 60 * 1000UL; // TTL profil threshold cloud (24 jam)
const unsigned long SCHEDULE_STALE_THRESHOLD_MS = 24 * 60 * 60 * 1000UL; // TTL jadwal cloud sebelum fallback ke lokal (24 jam)
const unsigned long CONTROL_SOURCE_LOSS_FAILSAFE_MS = 30 * 1000UL; // Grace period sebelum failsafe jika source kontrol hilang
const unsigned long FAILSAFE_TIMEOUT_MS = 2 * 60 * 60 * 1000UL; // Waktu koneksi hilang sebelum masuk mode failsafe (2 jam)
const unsigned long SD_RETRY_INTERVAL_MS = 5 * 60 * 1000UL;    // Interval mencoba re-inisialisasi SD Card (5 menit)
const unsigned long INITIAL_RETRY_DELAY_MS = 15 * 1000UL;       // Jeda awal sebelum mencoba koneksi ulang
const unsigned long MAX_RETRY_DELAY_MS = 5 * 60 * 1000UL;       // Jeda maksimum sebelum mencoba koneksi ulang
const unsigned long WIFI_RETRY_WHEN_GPRS_MS = 15 * 60 * 1000UL; // Interval mencoba WiFi saat terhubung via GPRS (15 menit)
const unsigned long MANUAL_OVERRIDE_DURATION_MS = 5 * 60 * 1000UL;  // Durasi override relay manual dari web (5 menit)
const unsigned long DEVICE_STATUS_CHECK_INTERVAL_MS = 5 * 60 * 1000UL; // Interval memeriksa status manual dari web API (5 menit)
const unsigned long DEVICE_STATUS_RESYNC_INTERVAL_MS = 5 * 60 * 1000UL; // Interval re-sync status relay ke server (5 menit)
const unsigned long RELAY_STATUS_SYNC_RETRY_MS = 1000UL;      // Jeda retry sinkron status relay agar retry tidak membanjiri loop
const unsigned long RTC_SYNC_RETRY_MS = 60 * 1000UL;          // Retry sinkron waktu jika percobaan sebelumnya gagal
const unsigned long CLIENT_TIME_SAMPLE_MAX_AGE_MS = 2 * 60 * 1000UL; // Sampel waktu dashboard hanya dipercaya singkat untuk bootstrap
const uint16_t MODEM_TIME_SYNC_MIN_TIMEOUT_MS = 2200U;        // Budget minimum untuk baca waktu operator lewat modem tanpa mengganggu kontrol
const uint32_t GPRS_SIM_STATUS_TIMEOUT_MS = 5000UL;           // Batas waktu cek status SIM sebelum fallback modem dianggap gagal
const uint32_t GPRS_NETWORK_ATTACH_TIMEOUT_MS = 15000UL;      // Batas waktu tunggu attach GPRS agar loop tidak tertahan terlalu lama
const bool AUTO_OTA_APPLY_ENABLED = false;                    // Update firmware otomatis tidak boleh memutus kontrol produksi

// --- Web Config Portal ---
const unsigned long PORTAL_TIMEOUT = 5 * 60 * 1000; // Timeout untuk portal konfigurasi (5 menit)

// -- [BARU] Default password untuk WebSerial Admin
const char DEFAULT_ADMIN_PASS[] PROGMEM = "change-me-admin-password";

// --- Watchdog Timer ---
const int WDT_TIMEOUT = 120; // Timeout watchdog dalam detik. Pastikan loop selesai < waktu ini.

// --- WebSocket Configuration ---
const unsigned long WEB_UPDATE_INTERVAL_MS = 5 * 1000UL; // Interval pengiriman status via WebSocket (5 detik)

#endif // CONFIG_H
