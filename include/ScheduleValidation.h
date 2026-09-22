#ifndef SCHEDULE_VALIDATION_H
#define SCHEDULE_VALIDATION_H

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include "RelayController.h"

namespace ScheduleValidation {

inline bool isTimeComponentValid(int hour, int minute)
{
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

inline bool isWindowNonZero(uint8_t startHour, uint8_t startMin, uint8_t endHour, uint8_t endMin)
{
    return startHour != endHour || startMin != endMin;
}

inline bool isRelayModeValid(char mode)
{
    return mode == '0' || mode == '1' || mode == '2';
}

inline bool isScheduleWindowValid(uint8_t startHour, uint8_t startMin, uint8_t endHour, uint8_t endMin)
{
    return isTimeComponentValid(startHour, startMin) &&
           isTimeComponentValid(endHour, endMin) &&
           isWindowNonZero(startHour, startMin, endHour, endMin);
}

inline bool isScheduleConfigValid(const ScheduleConfig& cfg)
{
    if (!cfg.active)
        return true;
    return isScheduleWindowValid(cfg.startHour, cfg.startMin, cfg.endHour, cfg.endMin) &&
           isRelayModeValid(cfg.r1Mode) &&
           isRelayModeValid(cfg.r2Mode) &&
           isRelayModeValid(cfg.r3Mode);
}

inline bool parseScheduleActiveFlag(JsonVariantConst value)
{
    if (value.is<bool>())
        return value.as<bool>();
    if (value.is<int>())
        return value.as<int>() == 1;
    if (value.is<const char*>()) {
        const char* raw = value.as<const char*>();
        if (!raw || raw[0] == '\0')
            return false;
        return (strcmp(raw, "1") == 0 ||
                strcmp(raw, "true") == 0 ||
                strcmp(raw, "TRUE") == 0 ||
                strcmp(raw, "True") == 0);
    }
    return false;
}

inline bool parseScheduleTimeValue(const char* raw, uint8_t& outHour, uint8_t& outMin)
{
    if (!raw || raw[0] == '\0')
        return false;

    int hour = 0;
    int minute = 0;
    char extra = '\0';
    if (sscanf(raw, "%d:%d%c", &hour, &minute, &extra) != 2)
        return false;
    if (!isTimeComponentValid(hour, minute))
        return false;

    outHour = static_cast<uint8_t>(hour);
    outMin = static_cast<uint8_t>(minute);
    return true;
}

inline bool parseScheduleRelayModes(const char* raw, char& r1, char& r2, char& r3)
{
    if (!raw || strlen(raw) != 3)
        return false;
    if (!isRelayModeValid(raw[0]) || !isRelayModeValid(raw[1]) || !isRelayModeValid(raw[2]))
        return false;

    r1 = raw[0];
    r2 = raw[1];
    r3 = raw[2];
    return true;
}

inline bool parseScheduleConfig(JsonObjectConst source, ScheduleConfig& outCfg)
{
    outCfg = {0, false, 0, 0, 0, 0, '2', '2', '2'};
    outCfg.id = source["id"] | 0;
    outCfg.active = parseScheduleActiveFlag(source["aktif"]);
    if (!outCfg.active)
        return true;

    if (!parseScheduleTimeValue(source["mulai"].as<const char*>(), outCfg.startHour, outCfg.startMin))
        return false;
    if (!parseScheduleTimeValue(source["selesai"].as<const char*>(), outCfg.endHour, outCfg.endMin))
        return false;
    if (!parseScheduleRelayModes(source["relay"].as<const char*>(), outCfg.r1Mode, outCfg.r2Mode, outCfg.r3Mode))
        return false;

    return isScheduleConfigValid(outCfg);
}

} // namespace ScheduleValidation

#endif // SCHEDULE_VALIDATION_H
