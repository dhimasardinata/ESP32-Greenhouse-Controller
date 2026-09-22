#ifndef SD_CARD_LOGGER_H
#define SD_CARD_LOGGER_H

#include "LCDDisplay.h"
#include <stdint.h> 
#include <time.h> // Perlu untuk time_t

struct SDCardInfo {
    uint64_t totalBytes;
    uint64_t usedBytes;
};

class SDCardLogger {
public:
    SDCardLogger(LCDDisplay& d, bool& sdOkFlag);
    bool begin();
    bool reInit();
    void closeFiles();

    void setBusy(bool state) { _isBusy = state; }
    bool isBusy() { return _isBusy; }
    
    void logData(const char* dt, float t, float h, float l, int rssi, bool isGprs,
                 bool r1, bool r2, bool r3, bool r4, bool fogStatus,
                 float tMin, float tMax, float hMin, float hMax,
                 const char* gatewayMode, const char* thresholdSource, const char* scheduleSource,
                 bool r1ScheduleActive, bool r2ScheduleActive, bool r3ScheduleActive,
                 int r1ScheduleId, int r2ScheduleId, int r3ScheduleId,
                 const char* r1Decision, const char* r2Decision, const char* r3Decision);

    // [UBAH] Parameter disederhanakan: Hapus MsgID dan RX Epoch
    void logQoS(const char* nodeId, unsigned long sendEpoch, size_t payloadSize, int rssiActive, int rssiNonActive);

    size_t getLogFileSize();
    SDCardInfo getStorageInfo();
    bool formatLog();

private:
    LCDDisplay& lcd_ref;
    bool& sdOk; 
    volatile bool _isBusy = false;
};

#endif // SD_CARD_LOGGER_H
