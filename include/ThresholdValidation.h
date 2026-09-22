#ifndef THRESHOLD_VALIDATION_H
#define THRESHOLD_VALIDATION_H

#include <math.h>
#include <string.h>

namespace ThresholdValidation {

constexpr float kTempMinAllowed = -20.0f;
constexpr float kTempMaxAllowed = 80.0f;
constexpr float kHumMinAllowed = 0.0f;
constexpr float kHumMaxAllowed = 100.0f;
constexpr float kLightMinAllowed = 0.0f;
constexpr float kLightMaxAllowed = 65535.0f;

inline bool isTemperatureRangeValid(float minValue, float maxValue)
{
    if (!isfinite(minValue) || !isfinite(maxValue))
        return false;
    return minValue >= kTempMinAllowed &&
           maxValue <= kTempMaxAllowed &&
           minValue < maxValue;
}

inline bool isHumidityRangeValid(float minValue, float maxValue)
{
    if (!isfinite(minValue) || !isfinite(maxValue))
        return false;
    return minValue >= kHumMinAllowed &&
           maxValue <= kHumMaxAllowed &&
           minValue < maxValue;
}

inline bool isLightRangeValid(float minValue, float maxValue)
{
    if (!isfinite(minValue) || !isfinite(maxValue))
        return false;
    return minValue >= kLightMinAllowed &&
           maxValue <= kLightMaxAllowed &&
           minValue < maxValue;
}

inline bool isRangeValidForName(const char* name, float minValue, float maxValue)
{
    if (!name)
        return false;
    if (strcmp(name, "Temperature") == 0)
        return isTemperatureRangeValid(minValue, maxValue);
    if (strcmp(name, "Humidity") == 0)
        return isHumidityRangeValid(minValue, maxValue);
    if (strcmp(name, "Light Intensity") == 0)
        return isLightRangeValid(minValue, maxValue);
    return false;
}

} // namespace ThresholdValidation

#endif // THRESHOLD_VALIDATION_H
