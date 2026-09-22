#include "SDCardLogger.h"
#include <SPI.h>
#include <SD.h>
#include "config.h"

#define DEBUG_PRINTLN(level, ...)        \
    do                                   \
    {                                    \
        if (DEBUG_LEVEL >= level)        \
        {                                \
            Serial.println(__VA_ARGS__); \
        }                                \
    } while (0)

// Variable Global File Handles
File logFile;
File qosFile;
int writeCounter = 0;
const int FLUSH_INTERVAL = 12;

SDCardLogger::SDCardLogger(LCDDisplay &d, bool &sdOkFlag)
    : lcd_ref(d), sdOk(sdOkFlag) {}

bool SDCardLogger::begin()
{
    lcd_ref.message(0, 1, "Init SD card...", true);
    sdOk = false;

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

    if (!SD.begin(SD_CS, SPI, 4000000))
    {
        lcd_ref.message(0, 2, "SD Mount Failed!", true);
        DEBUG_PRINTLN(1, "SD Card Mount Failed.");
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE)
    {
        lcd_ref.message(0, 2, "No SD Card!", true);
        DEBUG_PRINTLN(1, "No SD card detected.");
        return false;
    }

    lcd_ref.message(0, 2, "SD Card OK", true);
    DEBUG_PRINTLN(3, "SD Card initialized successfully.");

    // 1. Init Main Log
    logFile = SD.open("/log.csv", FILE_APPEND);
    if (!logFile)
    {
        DEBUG_PRINTLN(1, "Failed to open log.csv");
        return false;
    }
    if (logFile.size() == 0)
    {
        logFile.println(F("DateTime,Temperature,Humidity,Light,NetworkType,Signal,Relay1,Relay2,Relay3,Relay4,FogStatus,Tmin,Tmax,Hmin,Hmax,GatewayMode,ThresholdSource,ScheduleSource,R1ScheduleActive,R2ScheduleActive,R3ScheduleActive,R1ScheduleId,R2ScheduleId,R3ScheduleId,R1Decision,R2Decision,R3Decision"));
        logFile.flush();
    }

    // 2. Init QoS Log
    qosFile = SD.open("/qos.csv", FILE_APPEND);
    if (qosFile)
    {
        if (qosFile.size() == 0)
        {
            qosFile.println(F("RX_Time,Node_ID,TX_Time,Size_Bytes,RSSI_Act,RSSI_NonAct"));  
            qosFile.flush();
        }
    }

    sdOk = true;
    _isBusy = false;
    return true;
}

bool SDCardLogger::reInit()
{
    if (_isBusy) return false;
    DEBUG_PRINTLN(2, "Attempting to re-initialize SD Card...");
    if (logFile)
        logFile.close();
    if (qosFile)
        qosFile.close();
    SD.end();
    delay(100);
    return begin();
}

void SDCardLogger::logData(const char *dt, float t, float h, float l, int rssi, bool isGprs,
                           bool r1, bool r2, bool r3, bool r4, bool fogStatus,
                           float tMin, float tMax, float hMin, float hMax,
                           const char *gatewayMode, const char *thresholdSource, const char *scheduleSource,
                           bool r1ScheduleActive, bool r2ScheduleActive, bool r3ScheduleActive,
                           int r1ScheduleId, int r2ScheduleId, int r3ScheduleId,
                           const char *r1Decision, const char *r2Decision, const char *r3Decision)
{
    // [FIX] Cek apakah SD sedang sibuk (download)
    if (_isBusy) {
        DEBUG_PRINTLN(3, "[SD] Skipped logging due to active download.");
        return;
    }

    if (!sdOk || !logFile) {
        if (sdOk) { sdOk = false; reInit(); }
        return;
    }

    char line[520];
    const char *netType = isGprs ? "GPRS" : "WiFi";
    snprintf(line, sizeof(line), "%s,%.2f,%.1f,%.1f,%s,%d,%s,%s,%s,%s,%d,%.1f,%.1f,%.1f,%.1f,%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,%s,%s",
             dt, t, h, l, netType, rssi,
             r1 ? "ON" : "OFF", r2 ? "ON" : "OFF", r3 ? "ON" : "OFF", r4 ? "ON" : "OFF",
             fogStatus ? 1 : 0,
             tMin, tMax, hMin, hMax,
             gatewayMode ? gatewayMode : "N/A",
             thresholdSource ? thresholdSource : "N/A",
             scheduleSource ? scheduleSource : "N/A",
             r1ScheduleActive ? 1 : 0,
             r2ScheduleActive ? 1 : 0,
             r3ScheduleActive ? 1 : 0,
             r1ScheduleId,
             r2ScheduleId,
             r3ScheduleId,
             r1Decision ? r1Decision : "N/A",
             r2Decision ? r2Decision : "N/A",
             r3Decision ? r3Decision : "N/A");
    logFile.println(line);

    writeCounter++;
    if (writeCounter >= FLUSH_INTERVAL) {
        logFile.flush();
        writeCounter = 0;
    }
}

void SDCardLogger::logQoS(const char *nodeId, unsigned long sendEpoch, size_t payloadSize, int rssiActive, int rssiNonActive)
{
    if (_isBusy || !sdOk) return;

    if (!qosFile) {
        qosFile = SD.open("/qos.csv", FILE_APPEND);
        if (!qosFile) return;
    }

    // 1. Ambil Waktu RX (Sekarang)
    time_t now = time(nullptr);
    struct tm rxTm;
    char rxStr[20];
    if (localtime_r(&now, &rxTm))
    {
        strftime(rxStr, sizeof(rxStr), "%Y-%m-%d %H:%M:%S", &rxTm);
    }
    else
    {
        strcpy(rxStr, "N/A");
    }

    // 2. Konversi Waktu TX (Dari Node) ke String
    char txStr[20];
    if (sendEpoch > 0)
    {
        time_t txTime = (time_t)sendEpoch;
        struct tm txTm;
        if (localtime_r(&txTime, &txTm))
        {
            strftime(txStr, sizeof(txStr), "%Y-%m-%d %H:%M:%S", &txTm);
        }
        else
        {
            strcpy(txStr, "Invalid_Time");
        }
    }
    else
    {
        strcpy(txStr, "N/A");
    }

    char line[128];
    // [UBAH] Masukkan 2 nilai RSSI tersebut ke dalam format string CSV
    snprintf(line, sizeof(line), "%s,%s,%s,%u,%d,%d", 
         rxStr, 
         nodeId, 
         txStr, 
         (unsigned int)payloadSize,
         rssiActive,
         rssiNonActive);
         
    qosFile.println(line);
    qosFile.flush();
}

size_t SDCardLogger::getLogFileSize() {
    if (_isBusy) return 0; // Hindari akses saat busy
    if (logFile && sdOk) return logFile.size();
    return 0;
}

SDCardInfo SDCardLogger::getStorageInfo() {
    SDCardInfo info = {0, 0};
    if (sdOk && !_isBusy) {
        info.totalBytes = SD.cardSize();
        info.usedBytes = SD.usedBytes();
    }
    return info;
}

bool SDCardLogger::formatLog() {
    if (_isBusy) return false;
    
    if (logFile) logFile.close();
    if (qosFile) qosFile.close();
    SD.remove("/log.csv");
    SD.remove("/qos.csv");
    return begin();
}

void SDCardLogger::closeFiles() {
    if (logFile) logFile.close();
    if (qosFile) qosFile.close();
    DEBUG_PRINTLN(2, "[SD] Files closed for download access.");
}
