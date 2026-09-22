#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include "config.h"

class LCDDisplay {
public:
    LCDDisplay();
    void begin();
    // REFACTOR: Added rssi and isGprs parameters
    void update(const char* dt, float temp, float hum, float light, bool r1, bool r2, bool r3, bool fogStatus,
                float tMin, float tMax, float humMin, float humMax,
                bool netConnected, bool isDataStale, bool sdCardOk, bool isInFailSafe,
                int rssi, bool isGprs);
    void message(int col, int row, const char* msg, bool clearLine = false);
    void clear();
    
    // I2C availability check to prevent spam
    bool isAvailable();
    void checkAvailability();  // Periodic probe

private:
    LiquidCrystal_I2C lcd_i2c;
    bool m_available = false;
    unsigned long m_lastCheck = 0;
    static constexpr unsigned long CHECK_INTERVAL_MS = 5000;  // Re-check every 5 seconds
};

#endif // LCD_DISPLAY_H
