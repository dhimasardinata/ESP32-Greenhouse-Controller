#include "RelayController.h"
#include "config.h"
#include "ScheduleValidation.h"

#define DEBUG_PRINTLN(level, ...) do { if (DEBUG_LEVEL >= level) { Serial.println(__VA_ARGS__); } } while (0)

RelayController::RelayController(LCDDisplay& lcd) : lcd_ref(lcd) {}

void RelayController::begin() {
    pins[RELAY_EXHAUST] = RELAY_CH1;
    pins[RELAY_DEHUMIDIFIER] = RELAY_CH2;
    pins[RELAY_BLOWER] = RELAY_CH3;
    pins[RELAY_UNUSED] = RELAY_CH4;
    
    lcd_ref.message(0, 1, "Init Relays...", true);
    for (int i = 0; i < 4; ++i) {
        pinMode(pins[i], OUTPUT);
        digitalWrite(pins[i], HIGH);
        states[i] = false;
    }

    // --- Load Jadwal Local dari NVS ---
    preferences.begin("scheduler", true); // Read-only mode
    localSchedulesValid = preferences.getBool("local_valid", false);
    for(int i=0; i<MAX_SCHEDULES; i++) {
        char key[8]; snprintf(key, sizeof(key), "s%d", i);
        // Default: ID 0, Inaktif, 00:00-00:00, Mode Threshold ('2')
        ScheduleConfig def = {0, false, 0, 0, 0, 0, '2', '2', '2'};
        
        if(preferences.getBytesLength(key) == sizeof(ScheduleConfig)) {
            preferences.getBytes(key, &localSchedules[i], sizeof(ScheduleConfig));
            localSchedulesValid = true;
        } else {
            localSchedules[i] = def;
        }
    }
    preferences.end();

    preferences.begin("scheduler_cloud", true); // Read-only mode
    cloudSchedulesValid = preferences.getBool("cloud_valid", false);
    bool anyCloudStored = false;
    for(int i=0; i<MAX_SCHEDULES; i++) {
        char key[8]; snprintf(key, sizeof(key), "s%d", i);
        ScheduleConfig def = {0, false, 0, 0, 0, 0, '2', '2', '2'};
        if(preferences.getBytesLength(key) == sizeof(ScheduleConfig)) {
            preferences.getBytes(key, &cloudSchedules[i], sizeof(ScheduleConfig));
            anyCloudStored = true;
        } else {
            cloudSchedules[i] = def;
        }
    }
    if (!cloudSchedulesValid && anyCloudStored) {
        cloudSchedulesValid = true;
    }
    preferences.end();
    lastCloudScheduleSuccessMs = 0;

    DEBUG_PRINTLN(3, "Relays & Schedules Initialized.");
}

void RelayController::loadOverrides() {
    preferences.begin("relay-ovr", false);
    for (int i = 0; i < 3; i++) {
        char key_et[12]; snprintf(key_et, sizeof(key_et), "ovr_et_%d", i);
        char key_tgt[12]; snprintf(key_tgt, sizeof(key_tgt), "ovr_tgt_%d", i);
        manualOverrideActive[i] = false;
        manualOverrideTarget[i] = false;
        manualOverrideEndTime[i] = 0;
        if (preferences.isKey(key_et))
            preferences.remove(key_et);
        if (preferences.isKey(key_tgt))
            preferences.remove(key_tgt);
    }
    preferences.end();
}

void RelayController::saveOverride(RelayIndex rI) {
    if (rI >= 0 && rI < 3) {
        preferences.begin("relay-ovr", false);
        char key_et[12], key_tgt[12];
        snprintf(key_et, sizeof(key_et), "ovr_et_%d", rI);
        snprintf(key_tgt, sizeof(key_tgt), "ovr_tgt_%d", rI);
        
        if (manualOverrideActive[rI]) {
            preferences.putULong(key_et, manualOverrideEndTime[rI]);
            preferences.putBool(key_tgt, manualOverrideTarget[rI]);
        } else {
            if (preferences.isKey(key_et))
                preferences.remove(key_et);
            if (preferences.isKey(key_tgt))
                preferences.remove(key_tgt);
        }
        preferences.end();
    }
}

// --- CLOUD UPDATE (RAM + NVS) ---
// Jadwal cloud disimpan terpisah agar mode LOCAL tetap punya jadwal sendiri.
void RelayController::updateSchedulesFromCloud(const JsonArray& data) {
    ScheduleConfig parsedSchedules[MAX_SCHEDULES];
    ScheduleConfig def = {0, false, 0, 0, 0, 0, '2', '2', '2'};
    for (int i = 0; i < MAX_SCHEDULES; ++i) parsedSchedules[i] = def;
    int idx = 0;
    for (JsonObject s : data) {
        if (idx >= MAX_SCHEDULES) break;

        if (!ScheduleValidation::parseScheduleConfig(s, parsedSchedules[idx])) {
            Serial.println("[SCHED] Invalid cloud schedule payload ignored.");
            return;
        }
        idx++;
    }

    applyCloudSchedules(parsedSchedules, MAX_SCHEDULES);
}

void RelayController::applyCloudSchedules(const ScheduleConfig* schedules, size_t count) {
    ScheduleConfig def = {0, false, 0, 0, 0, 0, '2', '2', '2'};
    ScheduleConfig newSchedules[MAX_SCHEDULES];
    for (int i = 0; i < MAX_SCHEDULES; ++i) {
        newSchedules[i] = (schedules && static_cast<size_t>(i) < count) ? schedules[i] : def;
    }

    auto scheduleEqual = [](const ScheduleConfig& a, const ScheduleConfig& b) -> bool {
        return a.id == b.id &&
               a.active == b.active &&
               a.startHour == b.startHour &&
               a.startMin == b.startMin &&
               a.endHour == b.endHour &&
               a.endMin == b.endMin &&
               a.r1Mode == b.r1Mode &&
               a.r2Mode == b.r2Mode &&
               a.r3Mode == b.r3Mode;
    };

    bool changed = !cloudSchedulesValid;
    for (int i = 0; i < MAX_SCHEDULES && !changed; i++) {
        if (!scheduleEqual(newSchedules[i], cloudSchedules[i])) {
            changed = true;
        }
    }

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        cloudSchedules[i] = newSchedules[i];
    }
    cloudSchedulesValid = true;
    lastCloudScheduleSuccessMs = millis();

    // Persist hasil cloud ke NVS hanya jika berubah (hindari write flash berulang)
    if (changed) {
        preferences.begin("scheduler_cloud", false);
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            char key[8]; snprintf(key, sizeof(key), "s%d", i);
            preferences.putBytes(key, &cloudSchedules[i], sizeof(ScheduleConfig));
        }
        preferences.putBool("cloud_valid", true);
        preferences.end();
    }
}

bool RelayController::shouldUseLocalSchedules(bool forceLocal, bool preferCloudCache) const {
    if (forceLocal) return true;
    if (cloudSchedulesValid) {
        if (preferCloudCache) {
            // CLOUD mode: always prefer cached cloud schedules if they exist.
            return false;
        }
        if (lastCloudScheduleSuccessMs != 0) {
            const unsigned long now = millis();
            const bool cloudFresh = (now - lastCloudScheduleSuccessMs) <= SCHEDULE_STALE_THRESHOLD_MS;
            if (cloudFresh) return false;
        }
    }
    // Cloud stale/invalid: use local only if it's explicitly configured.
    if (localSchedulesValid) return true;
    // No local config: keep last-known cloud (even if stale) to avoid holes.
    if (cloudSchedulesValid) return false;
    // Last resort: use local defaults.
    return true;
}

bool RelayController::hasCloudScheduleProfile() const
{
    return cloudSchedulesValid;
}

bool RelayController::hasLocalScheduleProfile() const
{
    return localSchedulesValid;
}

bool RelayController::hasFreshCloudScheduleProfile(unsigned long now) const
{
    if (!cloudSchedulesValid || lastCloudScheduleSuccessMs == 0)
        return false;
    return (now - lastCloudScheduleSuccessMs) <= SCHEDULE_STALE_THRESHOLD_MS;
}

bool RelayController::hasAnyActiveSchedule(bool useLocalSchedules) const
{
    const ScheduleConfig* scheduleSource = useLocalSchedules ? localSchedules : cloudSchedules;
    for (int i = 0; i < MAX_SCHEDULES; ++i)
    {
        if (scheduleSource[i].active)
            return true;
    }
    return false;
}

// --- LOCAL SAVE (RAM + NVS) ---
// [FIX] Implementasi fungsi yang hilang
bool RelayController::saveSchedule(int index, const ScheduleConfig& cfg) {
    if(index < 0 || index >= MAX_SCHEDULES) return false;
    ScheduleConfig normalized = cfg;
    if (normalized.id <= 0)
    {
        const int existingId = localSchedules[index].id;
        normalized.id = existingId > 0 ? existingId : (index + 1);
    }
    if (!ScheduleValidation::isScheduleConfigValid(normalized)) return false;

    // Update RAM (Langsung efektif)
    localSchedules[index] = normalized;

    // Update NVS (Persisten setelah reboot)
    preferences.begin("scheduler", false); // Read-Write
    char key[8]; snprintf(key, sizeof(key), "s%d", index);
    preferences.putBytes(key, &normalized, sizeof(ScheduleConfig));
    preferences.putBool("local_valid", true);
    preferences.end();
    localSchedulesValid = true;
    
    DEBUG_PRINTLN(2, "Local Schedule saved to NVS.");
    return true;
}

const ScheduleConfig* RelayController::getSchedulesForMode(bool useLocalSchedules) const {
    if (!useLocalSchedules) {
        return cloudSchedules;
    }
    return localSchedules;
}

const char* RelayController::getScheduleSourceString(bool useLocalSchedules) const {
    if (!useLocalSchedules) {
        return "CLOUD";
    }
    return "EDGE";
}

const char* RelayController::getRelayDecisionSourceString(RelayIndex relayIndex) const {
    if (relayIndex < RELAY_EXHAUST || relayIndex > RELAY_BLOWER) return "UNKNOWN";

    switch (lastDecisionSource[relayIndex]) {
        case RELAY_DECISION_MANUAL:
            return "MANUAL";
        case RELAY_DECISION_SCHEDULE:
            return "SCHEDULE";
        case RELAY_DECISION_HOLD:
            return "HOLD";
        case RELAY_DECISION_THRESHOLD:
        default:
            return "THRESHOLD";
    }
}

bool RelayController::wasScheduleActiveForRelay(RelayIndex relayIndex) const {
    if (relayIndex < RELAY_EXHAUST || relayIndex > RELAY_BLOWER) return false;
    return lastScheduleActive[relayIndex];
}

int RelayController::getActiveScheduleIdForRelay(RelayIndex relayIndex) const {
    if (relayIndex < RELAY_EXHAUST || relayIndex > RELAY_BLOWER) return -1;
    return lastScheduleId[relayIndex];
}

bool RelayController::isScheduleActive(const ScheduleConfig& cfg, int currentHour, int currentMin) const {
    if (!cfg.active || !ScheduleValidation::isScheduleConfigValid(cfg)) return false;
    int nowMins = (currentHour * 60) + currentMin;
    int startMins = (cfg.startHour * 60) + cfg.startMin;
    int endMins = (cfg.endHour * 60) + cfg.endMin;

    if (startMins < endMins) {
        return (nowMins >= startMins && nowMins < endMins);
    } else {
        return (nowMins >= startMins || nowMins < endMins);
    }
}

bool RelayController::hasAnyExplicitActiveSchedule(int currentHour, int currentMin, bool useLocalSchedules) const
{
    const ScheduleConfig* scheduleSource = useLocalSchedules ? localSchedules : cloudSchedules;
    for (int i = 0; i < MAX_SCHEDULES; ++i)
    {
        if (!isScheduleActive(scheduleSource[i], currentHour, currentMin))
            continue;
        if (scheduleSource[i].r1Mode != '2' ||
            scheduleSource[i].r2Mode != '2' ||
            scheduleSource[i].r3Mode != '2')
        {
            return true;
        }
    }
    return false;
}

bool RelayController::updateSingleRelayState(RelayIndex rI, float hum_val, float hMn, float hMx, 
                                             float temp_val, float tMn, float tMx,
                                             int currentHour, int currentMin, bool fog,
                                             bool useLocalSchedules, bool allowScheduleEvaluation,
                                             bool allowThresholdEvaluation) {
    if (rI < RELAY_EXHAUST || rI > RELAY_BLOWER) return false;

    bool targetState = false;
    bool logicDetermined = false;

    lastDecisionSource[rI] = allowThresholdEvaluation ? RELAY_DECISION_THRESHOLD : RELAY_DECISION_HOLD;
    lastScheduleActive[rI] = false;
    lastScheduleId[rI] = -1;

    // 1. Schedule
    char scheduledMode = '2'; // Default Threshold
    if (allowScheduleEvaluation) {
        ScheduleConfig activeSchedules[MAX_SCHEDULES];
        const ScheduleConfig* scheduleSource = useLocalSchedules ? localSchedules : cloudSchedules;
        for (int i = 0; i < MAX_SCHEDULES; i++) {
            activeSchedules[i] = scheduleSource[i];
        }

        for(int i=0; i<MAX_SCHEDULES; i++) {
            if(isScheduleActive(activeSchedules[i], currentHour, currentMin)) {
                lastScheduleActive[rI] = true;
                lastScheduleId[rI] = activeSchedules[i].id;
                if (rI == RELAY_EXHAUST) scheduledMode = activeSchedules[i].r1Mode;
                else if (rI == RELAY_DEHUMIDIFIER) scheduledMode = activeSchedules[i].r2Mode;
                else if (rI == RELAY_BLOWER) scheduledMode = activeSchedules[i].r3Mode;
                break; 
            }
        }
    }

    if (scheduledMode == '1') {
        targetState = true;
        logicDetermined = true;
        lastDecisionSource[rI] = RELAY_DECISION_SCHEDULE;
    }
    else if (scheduledMode == '0') {
        targetState = false;
        logicDetermined = true;
        lastDecisionSource[rI] = RELAY_DECISION_SCHEDULE;
    }

    // 2. Sensor Logic (Threshold)
    if (!logicDetermined && allowThresholdEvaluation) {
        auto applyDeadband = [&](float val, float vMin, float vMax) -> bool {
            if (states[rI]) {
                return (val > vMin);
            }
            return (val >= vMax);
        };

        switch (rI) {
            case RELAY_EXHAUST:
            case RELAY_DEHUMIDIFIER:
                // Fog tetap boleh memaksa relay langsung nyala.
                if (fog) {
                    targetState = true;
                } else {
                    targetState = applyDeadband(hum_val, hMn, hMx);
                }
                break;
            case RELAY_BLOWER:
                targetState = applyDeadband(temp_val, tMn, tMx);
                break;
        }
    }
    else if (!logicDetermined)
    {
        targetState = states[rI];
        logicDetermined = true;
        lastDecisionSource[rI] = RELAY_DECISION_HOLD;
    }

    if (states[rI] != targetState) {
        states[rI] = targetState;
        digitalWrite(pins[rI], states[rI] ? LOW : HIGH);
        return true;
    }
    return false;
}

void RelayController::setManualOverride(RelayIndex rI, bool desiredState, unsigned long duration) {
    if (rI < RELAY_EXHAUST || rI > RELAY_BLOWER) return;
    (void)desiredState;
    (void)duration;
    const bool hadOverride = manualOverrideActive[rI] ||
                             manualOverrideTarget[rI] ||
                             manualOverrideEndTime[rI] != 0;
    manualOverrideActive[rI] = false;
    manualOverrideTarget[rI] = false;
    manualOverrideEndTime[rI] = 0;
    if (hadOverride)
        saveOverride(rI);
}

void RelayController::ensureRelay4Off() {
    if (states[RELAY_UNUSED]) {
        digitalWrite(pins[RELAY_UNUSED], HIGH);
        states[RELAY_UNUSED] = false;
    }
}

void RelayController::forceSafeState() {
    for (int i = 0; i < 3; ++i) {
        const bool hadOverride = manualOverrideActive[i] ||
                                 manualOverrideTarget[i] ||
                                 manualOverrideEndTime[i] != 0;
        manualOverrideActive[i] = false;
        manualOverrideTarget[i] = false;
        manualOverrideEndTime[i] = 0;
        if (hadOverride)
            saveOverride((RelayIndex)i);
        if (states[i]) {
            digitalWrite(pins[i], HIGH);
            states[i] = false;
        }
    }
    ensureRelay4Off();
}
