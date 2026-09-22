#ifndef RTC_MANAGER_H
#define RTC_MANAGER_H

#include <RTClib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "LCDDisplay.h"
#include "MyNetworkManager.h"

enum class RtcSyncResult : uint8_t {
    None = 0,
    Ntp,
    ModemNetwork,
    Http,
    ClientBootstrap
};

enum class RtcTimeSource : uint8_t {
    None = 0,
    RtcHardware,
    ModemNetwork,
    HttpApi,
    ClientBootstrap,
    Ntp
};

class RTCManager {
public:
    RTCManager(LCDDisplay& d, MyNetworkManager& n);
    bool begin();
    void update();
    RtcSyncResult checkAndSyncOnDrift(uint16_t timeoutMs = 0);
    const char* getTime();
    bool isHardwareOk() { return rtcHardwareOk; }
    bool isTimeSet() { return timeIsSet; } // <-- [BARU] Fungsi untuk mengecek apakah waktu sudah valid
    void noteClientEpochSample(uint32_t epochSec, unsigned long receivedAtMs = 0);
    bool syncFromRecentClientEpoch(unsigned long nowMs = 0);
    const char* getLastSyncSourceString() const;
    const char* getConfiguredTimezoneName() const { return configuredTimezoneName; }
    const char* getConfiguredTimezonePosix() const { return configuredTimezonePosix; }
    const char* getConfiguredUtcOffset() const { return configuredUtcOffset; }
    const char* getLastValidatedTimezone() const { return lastValidatedTimezone; }
    const char* getLastValidatedUtcOffset() const { return lastValidatedUtcOffset; }
    uint32_t getLastSyncEpochUtc() const { return lastSyncEpochUtc; }
    unsigned long getLastSyncMillis() const { return lastSyncMillis; }
    bool wasLastSyncAuthoritative() const { return lastSyncAuthoritative; }
    const char* getPrimaryNtpServer() const { return primaryNtpServer; }
    const char* getSecondaryNtpServer() const { return secondaryNtpServer; }
    const char* getTertiaryNtpServer() const { return tertiaryNtpServer; }
    const char* getLastNtpServer() const { return lastNtpServer; }

private:
    char dateTime[20] = "YYYY-MM-DD HH:MM:SS";
    bool rtcHardwareOk = false;
    bool timeIsSet = false; // <-- [BARU] Lacak status sinkronisasi waktu
    uint32_t lastClientEpochSec = 0;
    unsigned long lastClientEpochReceivedMs = 0;
    RtcTimeSource lastSyncSource = RtcTimeSource::None;
    uint32_t lastSyncEpochUtc = 0;
    unsigned long lastSyncMillis = 0;
    bool lastSyncAuthoritative = false;
    char configuredTimezoneName[40] = {0};
    char configuredTimezonePosix[16] = {0};
    char configuredUtcOffset[8] = {0};
    char lastValidatedTimezone[40] = {0};
    char lastValidatedUtcOffset[8] = {0};
    char primaryNtpServer[48] = {0};
    char secondaryNtpServer[48] = {0};
    char tertiaryNtpServer[48] = {0};
    char lastNtpServer[48] = {0};
    LCDDisplay& lcd_ref;
    MyNetworkManager& netManager;
    
    RTC_DS3231 rtc;
    WiFiUDP ntpUDP;
    NTPClient timeClient;

    bool syncNTP();
    bool syncModemNetworkTime();
    bool syncHttpTime(uint16_t timeoutMs = 0);
    bool applyEpoch(time_t epoch, const char* lcdMessage, const char* debugMessage);
    bool trySyncNtpServer(const char* serverName);
    const char* getNtpServerForIndex(uint8_t index) const;
    void recordSyncMetadata(RtcTimeSource source,
                            uint32_t epochSec,
                            const char* timezoneName,
                            const char* utcOffset,
                            bool authoritative);
    uint8_t nextNtpServerIndex = 0;
};

#endif // RTC_MANAGER_H
