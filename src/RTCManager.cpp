///////////////////////////////////////////////////////////////////////////////////
// File: RTCManager.cpp (REVISED & FIXED)
//
// Description:
// - [FIX] Memperbaiki logika timezone. Jam internal ESP32 sekarang disetel ke UTC,
//   dan variabel environment TZ diatur untuk konversi lokal yang benar.
// - [REFACTOR] Mengimplementasikan fallback ke NTP jika modul RTC tidak ada.
// - Sekarang mengelola waktu sistem (jam internal ESP32) selain RTC fisik.
// - Memastikan perangkat selalu memiliki waktu yang valid jika terhubung ke internet.
///////////////////////////////////////////////////////////////////////////////////

#include "RTCManager.h"
#include <Wire.h>
#include "config.h" // Diperlukan untuk pin dan konstanta
#include "CryptoUtils.h"
#include <time.h>    // Diperlukan untuk mengelola jam internal ESP32

// Macro untuk debug
#define DEBUG_PRINTLN(level, ...) do { if (DEBUG_LEVEL >= level) { Serial.println(__VA_ARGS__); } } while (0)

RTCManager::RTCManager(LCDDisplay& d, MyNetworkManager& n)
    : lcd_ref(d), netManager(n), timeClient(ntpUDP, "pool.ntp.org") {} // Inisialisasi NTPClient di sini

void RTCManager::recordSyncMetadata(RtcTimeSource source,
                                    uint32_t epochSec,
                                    const char* timezoneName,
                                    const char* utcOffset,
                                    bool authoritative) {
    lastSyncSource = source;
    lastSyncEpochUtc = epochSec;
    lastSyncMillis = millis();
    lastSyncAuthoritative = authoritative;
    strncpy(lastValidatedTimezone, timezoneName ? timezoneName : "", sizeof(lastValidatedTimezone) - 1);
    lastValidatedTimezone[sizeof(lastValidatedTimezone) - 1] = '\0';
    strncpy(lastValidatedUtcOffset, utcOffset ? utcOffset : "", sizeof(lastValidatedUtcOffset) - 1);
    lastValidatedUtcOffset[sizeof(lastValidatedUtcOffset) - 1] = '\0';
    CryptoUtils::setReplaySkewWindow(authoritative
                                         ? CryptoUtils::ENCRYPTED_REPLAY_WINDOW_STRICT_SEC
                                         : CryptoUtils::ENCRYPTED_REPLAY_WINDOW_SOFT_SEC);
}

const char* RTCManager::getLastSyncSourceString() const {
    switch (lastSyncSource) {
    case RtcTimeSource::RtcHardware:
        return "RTC_HARDWARE";
    case RtcTimeSource::ModemNetwork:
        return "MODEM_NETWORK";
    case RtcTimeSource::HttpApi:
        return "HTTP_API";
    case RtcTimeSource::ClientBootstrap:
        return "CLIENT_BOOTSTRAP";
    case RtcTimeSource::Ntp:
        return "NTP";
    case RtcTimeSource::None:
    default:
        return "NONE";
    }
}

bool RTCManager::applyEpoch(time_t epoch, const char* lcdMessage, const char* debugMessage) {
    if (epoch <= 1672531200) return false;

    struct timeval tv = { .tv_sec = epoch };
    settimeofday(&tv, nullptr);
    timeIsSet = true;

    if (rtcHardwareOk) {
        rtc.adjust(DateTime(static_cast<uint32_t>(epoch)));
    }

    update();
    if (lcdMessage && lcdMessage[0] != '\0') {
        lcd_ref.message(0, 3, lcdMessage, true);
    }
    if (debugMessage && debugMessage[0] != '\0') {
        DEBUG_PRINTLN(3, debugMessage);
    }
    return true;
}

bool RTCManager::begin() {
    lcd_ref.message(0, 1, "Init Time Mgr...", true);
    CryptoUtils::setReplaySkewWindow(CryptoUtils::ENCRYPTED_REPLAY_WINDOW_SOFT_SEC);

    strncpy(configuredTimezoneName, GATEWAY_TIMEZONE_NAME, sizeof(configuredTimezoneName) - 1);
    configuredTimezoneName[sizeof(configuredTimezoneName) - 1] = '\0';
    strncpy(configuredTimezonePosix, GATEWAY_TIMEZONE_POSIX, sizeof(configuredTimezonePosix) - 1);
    configuredTimezonePosix[sizeof(configuredTimezonePosix) - 1] = '\0';
    strncpy(configuredUtcOffset, GATEWAY_TIMEZONE_OFFSET, sizeof(configuredUtcOffset) - 1);
    configuredUtcOffset[sizeof(configuredUtcOffset) - 1] = '\0';
    strncpy(primaryNtpServer, PRIMARY_NTP_SERVER, sizeof(primaryNtpServer) - 1);
    primaryNtpServer[sizeof(primaryNtpServer) - 1] = '\0';
    strncpy(secondaryNtpServer, SECONDARY_NTP_SERVER, sizeof(secondaryNtpServer) - 1);
    secondaryNtpServer[sizeof(secondaryNtpServer) - 1] = '\0';
    strncpy(tertiaryNtpServer, TERTIARY_NTP_SERVER, sizeof(tertiaryNtpServer) - 1);
    tertiaryNtpServer[sizeof(tertiaryNtpServer) - 1] = '\0';

    // <-- [FIX] Atur environment variable Timezone ke Waktu Indonesia Barat (GMT+7)
    // Format "WIB-7" adalah standar POSIX, yang artinya 7 jam di sebelah timur UTC.
    setenv("TZ", configuredTimezonePosix, 1);
    tzset(); // Terapkan pengaturan timezone
    timeClient.setPoolServerName(primaryNtpServer);
    timeClient.setTimeOffset(0);

    // Coba inisialisasi RTC fisik
    if (!rtc.begin()) {
        lcd_ref.message(0, 2, "RTC HW Failed!", true);
        DEBUG_PRINTLN(1, "RTC hardware not found. Will rely on NTP.");
        rtcHardwareOk = false;
    } else {
        rtcHardwareOk = true;
        DEBUG_PRINTLN(3, "RTC hardware found.");

        if (rtc.lostPower() || rtc.now().year() < 2023) {
            lcd_ref.message(0, 2, "RTC needs sync", true);
            DEBUG_PRINTLN(2, "RTC power was lost or time is invalid.");
        } else {
            // Jika RTC valid, langsung atur jam internal ESP32 dari RTC
            DateTime now = rtc.now();
            time_t t = now.unixtime();
            struct timeval tv = { .tv_sec = t };
            settimeofday(&tv, nullptr); // Atur jam sistem ke UTC
            timeIsSet = true;
            recordSyncMetadata(RtcTimeSource::RtcHardware,
                               static_cast<uint32_t>(t),
                               configuredTimezoneName,
                               configuredUtcOffset,
                               false);
            lcd_ref.message(0, 2, "RTC Time OK", true);
            DEBUG_PRINTLN(3, "System time set from RTC.");
        }
    }

    update(); // Perbarui string waktu internal untuk tampilan awal
    return rtcHardwareOk;
}

void RTCManager::update() {
    time_t now_epoch;
    // Ambil waktu dari sumber terbaik yang tersedia
    if (rtcHardwareOk) {
        // Prioritaskan RTC jika ada, karena lebih andal saat offline
        DateTime rtcNow = rtc.now();
        now_epoch = rtcNow.unixtime();
    } else {
        // Jika tidak ada RTC, gunakan jam internal ESP32
        now_epoch = time(nullptr);
    }
    
    // Format string dateTime dari epoch time
    if (now_epoch > 1672531200) { // Cek jika waktu valid (setelah 1 Jan 2023)
        timeIsSet = true;
        struct tm timeinfo;
        localtime_r(&now_epoch, &timeinfo); // localtime_r akan menggunakan TZ env var
        strftime(dateTime, sizeof(dateTime), "%Y-%m-%d %H:%M:%S", &timeinfo);
    } else {
        timeIsSet = false;
        // Jika waktu tidak valid, tetap tampilkan placeholder
        strncpy(dateTime, "YYYY-MM-DD HH:MM:SS", sizeof(dateTime));
    }
}

const char* RTCManager::getTime() {
    // Fungsi update sekarang menangani logika dari mana waktu diambil
    update();
    return dateTime;
}


RtcSyncResult RTCManager::checkAndSyncOnDrift(uint16_t timeoutMs) {
    if (netManager.isWiFiConnected() && syncNTP()) {
        return RtcSyncResult::Ntp;
    }

    if (timeoutMs >= MODEM_TIME_SYNC_MIN_TIMEOUT_MS &&
        netManager.isGprsConnected() &&
        syncModemNetworkTime()) {
        return RtcSyncResult::ModemNetwork;
    }

    if (netManager.isConnected() && syncHttpTime(timeoutMs)) {
        return RtcSyncResult::Http;
    }

    if (!timeIsSet && syncFromRecentClientEpoch(millis())) {
        return RtcSyncResult::ClientBootstrap;
    }

    return RtcSyncResult::None;
}

void RTCManager::noteClientEpochSample(uint32_t epochSec, unsigned long receivedAtMs) {
    if (epochSec <= 1672531200UL) return;

    if (receivedAtMs == 0) {
        receivedAtMs = millis();
    }

    lastClientEpochSec = epochSec;
    lastClientEpochReceivedMs = receivedAtMs;
}

bool RTCManager::syncFromRecentClientEpoch(unsigned long nowMs) {
    if (nowMs == 0) {
        nowMs = millis();
    }

    if (lastClientEpochSec <= 1672531200UL || lastClientEpochReceivedMs == 0) {
        return false;
    }

    if ((nowMs - lastClientEpochReceivedMs) > CLIENT_TIME_SAMPLE_MAX_AGE_MS) {
        return false;
    }

    const uint32_t adjustedEpoch = lastClientEpochSec + ((nowMs - lastClientEpochReceivedMs) / 1000UL);
    lcd_ref.message(0, 3, "Syncing Client Time...", true);
    if (!applyEpoch(static_cast<time_t>(adjustedEpoch),
                    "Client Time OK",
                    "Time synced via dashboard client epoch.")) {
        return false;
    }
    recordSyncMetadata(RtcTimeSource::ClientBootstrap,
                       adjustedEpoch,
                       configuredTimezoneName,
                       configuredUtcOffset,
                       false);
    return true;
}

bool RTCManager::trySyncNtpServer(const char* serverName) {
    if (!serverName || serverName[0] == '\0')
        return false;

    strncpy(lastNtpServer, serverName, sizeof(lastNtpServer) - 1);
    lastNtpServer[sizeof(lastNtpServer) - 1] = '\0';

    lcd_ref.message(0, 3, "Syncing NTP...", true);
    timeClient.end();
    timeClient.setPoolServerName(serverName);
    timeClient.begin();

    // Jam sistem harus tetap UTC. Konversi lokal ditangani TZ POSIX.
    timeClient.setTimeOffset(0);

    if (!timeClient.forceUpdate())
        return false;

    const unsigned long epochTime = timeClient.getEpochTime();
    if (epochTime <= 1672531200UL)
        return false;

    if (!applyEpoch(static_cast<time_t>(epochTime),
                    "NTP Sync OK",
                    "Time synced via NTP. System clock set to UTC.")) {
        return false;
    }

    recordSyncMetadata(RtcTimeSource::Ntp,
                       static_cast<uint32_t>(epochTime),
                       configuredTimezoneName,
                       configuredUtcOffset,
                       true);
    return true;
}

bool RTCManager::syncNTP() {
    const char* serverName = getNtpServerForIndex(nextNtpServerIndex);
    nextNtpServerIndex = (nextNtpServerIndex + 1) % 3;
    if (trySyncNtpServer(serverName))
        return true;

    lcd_ref.message(0, 3, "NTP Sync Failed", true);
    DEBUG_PRINTLN(1, "NTP sync failed.");
    return false;
}

bool RTCManager::syncModemNetworkTime() {
    ModemTimeInfo info;
    if (!netManager.fetchModemTimeInfo(info))
        return false;

    if (strcmp(info.utcOffset, configuredUtcOffset) != 0) {
        lcd_ref.message(0, 3, "Modem TZ Mismatch", true);
        DEBUG_PRINTLN(1, "Modem network time rejected: UTC offset mismatch.");
        return false;
    }

    lcd_ref.message(0, 3, "Syncing Modem Time...", true);
    if (!applyEpoch(static_cast<time_t>(info.epoch),
                    "Modem Time OK",
                    "Time synced via modem network clock.")) {
        return false;
    }

    recordSyncMetadata(RtcTimeSource::ModemNetwork,
                       info.epoch,
                       configuredTimezoneName,
                       info.utcOffset,
                       true);
    return true;
}

const char* RTCManager::getNtpServerForIndex(uint8_t index) const {
    const char* serverList[3] = {primaryNtpServer, secondaryNtpServer, tertiaryNtpServer};
    for (uint8_t offset = 0; offset < 3; ++offset) {
        const uint8_t current = (index + offset) % 3;
        const char* server = serverList[current];
        if (server && server[0] != '\0')
            return server;
    }
    return primaryNtpServer;
}

bool RTCManager::syncHttpTime(uint16_t timeoutMs) {
    if (timeoutMs == 0)
        timeoutMs = HTTP_REQUEST_TIMEOUT_MS;
    lcd_ref.message(0, 3, "Syncing HTTP Time...", true);
    HttpTimeInfo info;
    if (!netManager.fetchHttpTimeInfo(info, timeoutMs)) {
        lcd_ref.message(0, 3, "HTTP Time Failed", true);
        DEBUG_PRINTLN(1, "HTTP time sync failed.");
        return false;
    }
    
    if (strcmp(info.timezone, configuredTimezoneName) != 0) {
        lcd_ref.message(0, 3, "HTTP TZ Mismatch", true);
        DEBUG_PRINTLN(1, "HTTP time sync rejected: timezone mismatch.");
        return false;
    }
    if (strcmp(info.utcOffset, configuredUtcOffset) != 0) {
        lcd_ref.message(0, 3, "HTTP Offset Bad", true);
        DEBUG_PRINTLN(1, "HTTP time sync rejected: UTC offset mismatch.");
        return false;
    }

    if (info.epoch > 1672531200) { // Cek apakah waktu valid
        if (applyEpoch(static_cast<time_t>(info.epoch),
                       "HTTP Time OK",
                       "Time synced via HTTP API. System clock set to UTC.")) {
            recordSyncMetadata(RtcTimeSource::HttpApi,
                               info.epoch,
                               info.timezone,
                               info.utcOffset,
                               true);
            return true;
        }
    }
    
    lcd_ref.message(0, 3, "HTTP Time Failed", true);
    DEBUG_PRINTLN(1, "HTTP time sync failed.");
    return false;
}
