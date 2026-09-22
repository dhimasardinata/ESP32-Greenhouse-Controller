#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "LCDDisplay.h"
#include "config.h" 
#include <Preferences.h> 
#include <ArduinoJson.h>

#define MAX_SCHEDULES 4 

enum RelayDecisionSource : uint8_t {
    RELAY_DECISION_MANUAL = 0,
    RELAY_DECISION_SCHEDULE,
    RELAY_DECISION_THRESHOLD,
    RELAY_DECISION_HOLD
};

// Mode Relay: 0=OFF, 1=ON, 2=THRESHOLD
struct ScheduleConfig {
    int id;
    bool active;           
    uint8_t startHour;
    uint8_t startMin;
    uint8_t endHour;
    uint8_t endMin;
    
    char r1Mode; // '0', '1', '2'
    char r2Mode; 
    char r3Mode; 
};

class RelayController {
public:
    RelayController(LCDDisplay& lcd);
    void begin();
    
    bool updateSingleRelayState(RelayIndex rI, float hum_val, float hMn, float hMx, 
                                float temp_val, float tMn, float tMx, 
                                int currentHour, int currentMin, bool fog,
                                bool useLocalSchedules, bool allowScheduleEvaluation,
                                bool allowThresholdEvaluation = true);
    
    void ensureRelay4Off();
    void setManualOverride(RelayIndex relayIndex, bool desiredState, unsigned long duration);
    void forceSafeState();
    void loadOverrides();
    
    // --- Schedule Management ---
    void updateSchedulesFromCloud(const JsonArray& data);
    void applyCloudSchedules(const ScheduleConfig* schedules, size_t count);
    
    // [FIX] Method ini dikembalikan untuk Mode Local
    bool saveSchedule(int index, const ScheduleConfig& cfg);
    
    const ScheduleConfig* getSchedulesForMode(bool useLocalSchedules) const;
    const char* getScheduleSourceString(bool useLocalSchedules) const;
    bool shouldUseLocalSchedules(bool forceLocal, bool preferCloudCache) const;
    bool hasLocalScheduleProfile() const;
    bool hasCloudScheduleProfile() const;
    bool hasFreshCloudScheduleProfile(unsigned long now) const;
    bool hasAnyActiveSchedule(bool useLocalSchedules) const;
    bool hasAnyExplicitActiveSchedule(int currentHour, int currentMin, bool useLocalSchedules) const;
    const char* getRelayDecisionSourceString(RelayIndex relayIndex) const;
    bool wasScheduleActiveForRelay(RelayIndex relayIndex) const;
    int getActiveScheduleIdForRelay(RelayIndex relayIndex) const;

    bool getR1() { return states[RELAY_EXHAUST]; }
    bool getR2() { return states[RELAY_DEHUMIDIFIER]; }
    bool getR3() { return states[RELAY_BLOWER]; }
    bool getR4() { return states[RELAY_UNUSED]; }

private:
    void saveOverride(RelayIndex relayIndex);
    bool isScheduleActive(const ScheduleConfig& cfg, int currentHour, int currentMin) const;

    int pins[4];
    bool states[4] = {false};
    bool manualOverrideActive[3] = {false};
    bool manualOverrideTarget[3] = {false};
    unsigned long manualOverrideEndTime[3] = {0};
    
    ScheduleConfig localSchedules[MAX_SCHEDULES];
    ScheduleConfig cloudSchedules[MAX_SCHEDULES];
    bool cloudSchedulesValid = false;
    unsigned long lastCloudScheduleSuccessMs = 0;
    bool localSchedulesValid = false;
    uint8_t lastDecisionSource[3] = {RELAY_DECISION_THRESHOLD, RELAY_DECISION_THRESHOLD, RELAY_DECISION_THRESHOLD};
    bool lastScheduleActive[3] = {false, false, false};
    int lastScheduleId[3] = {-1, -1, -1};

    LCDDisplay& lcd_ref;
    Preferences preferences;
};

#endif // RELAY_CONTROLLER_H
