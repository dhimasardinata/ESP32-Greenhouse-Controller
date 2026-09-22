///////////////////////////////////////////////////////////////////////////////////
// File: LCDDisplay.cpp
//
// Description: Controls I2C LCD 20x4 display with robust error handling.
//              - Probes I2C bus before writes to prevent spam errors
//              - Gracefully handles LCD disconnection/reconnection
///////////////////////////////////////////////////////////////////////////////////

#include "LCDDisplay.h"
#include <Arduino.h>
#include "config.h"

#define DEBUG_PRINTLN(level, ...) do { if (DEBUG_LEVEL >= level) { Serial.println(__VA_ARGS__); } } while (0)

/**
 * Constructor - initializes LCD object with address and dimensions
 */
LCDDisplay::LCDDisplay() : lcd_i2c(LCD_ADDR, 20, 4) {}

/**
 * Initialize LCD display with I2C availability check
 */
void LCDDisplay::begin() {
    checkAvailability();
    
    if (m_available) {
        lcd_i2c.init();
        lcd_i2c.backlight();
        lcd_i2c.setCursor(0, 0);
        lcd_i2c.print(F("Relay Ctrl Loading.."));
        DEBUG_PRINTLN(3, "[LCD] Initialized successfully");
    } else {
        DEBUG_PRINTLN(1, "[LCD] Not detected at 0x27 - display disabled");
    }
}

/**
 * Check if LCD is available on I2C bus
 */
void LCDDisplay::checkAvailability() {
    Wire.beginTransmission(LCD_ADDR);
    uint8_t error = Wire.endTransmission();
    
    bool wasAvailable = m_available;
    m_available = (error == 0);
    m_lastCheck = millis();
    
    // Log state changes
    if (m_available && !wasAvailable) {
        DEBUG_PRINTLN(2, "[LCD] Detected - enabling display");
    } else if (!m_available && wasAvailable) {
        DEBUG_PRINTLN(2, "[LCD] Lost - disabling display");
    }
}

/**
 * Returns true if LCD is currently available
 */
bool LCDDisplay::isAvailable() {
    // Periodic re-check
    if (millis() - m_lastCheck >= CHECK_INTERVAL_MS) {
        checkAvailability();
    }
    return m_available;
}

/**
 * Clear entire display
 */
void LCDDisplay::clear() {
    if (!isAvailable()) return;
    lcd_i2c.clear();
}

/**
 * Display message at specific location
 */
void LCDDisplay::message(int col, int row, const char* msg, bool clearLine) {
    if (!isAvailable()) return;
    
    if (clearLine) {
        lcd_i2c.setCursor(0, row);
        lcd_i2c.print(F("                    ")); // 20 spaces
    }
    lcd_i2c.setCursor(col, row);

    char buf[21];
    snprintf(buf, sizeof(buf), "%-20s", msg);
    lcd_i2c.print(buf);

    DEBUG_PRINTLN(3, msg);
}

/**
 * Full display update with all status information
 */
void LCDDisplay::update(const char* dt, float temp, float hum, float light, bool r1, bool r2, bool r3, bool fogStatus,
                        float tMin, float tMax, float humMin, float humMax,
                        bool netConnected, bool isDataStale, bool sdCardOkLocal, bool isInFailSafe,
                        int rssi, bool isGprs) {
    
    if (!isAvailable()) return;
    
    char buf[21];
    lcd_i2c.clear();

    // Row 1: Time and network status
    if (isInFailSafe) {
        snprintf(buf, sizeof(buf), "** FAILSAFE ** %-8s", dt + 11);
    } else {
        const char* netType = isGprs ? "GP" : "WF";
        const char* netStatus;
        if (netConnected) {
            netStatus = isDataStale ? "ST" : "OK";
        } else {
            netStatus = "OF";
        }
        snprintf(buf, sizeof(buf), "%-8s %s:%s [%d]", dt + 11, netType, netStatus, rssi);
    }
    lcd_i2c.setCursor(0, 0);
    lcd_i2c.print(buf);

    // Row 2: Sensor data
    snprintf(buf, sizeof(buf), "T:%.1fC H:%.0f%% L:%.0f", temp, hum, light);
    lcd_i2c.setCursor(0, 1);
    lcd_i2c.print(buf);

    // Row 3: Relay status + fog indicator
    snprintf(buf, sizeof(buf), "Ex:%c Dh:%c Bl:%c f=%d",
             r1 ? 'Y' : 'N',
             r2 ? 'Y' : 'N',
             r3 ? 'Y' : 'N',
             fogStatus ? 1 : 0);
    lcd_i2c.setCursor(0, 2);
    lcd_i2c.print(buf);

    // Row 4: Thresholds
    snprintf(buf, sizeof(buf), "T:%.0f-%.0f H:%.0f-%.0f", tMin, tMax, humMin, humMax);
    lcd_i2c.setCursor(0, 3);
    lcd_i2c.print(buf);
}
