#ifndef SENSOR_NORMALIZATION_H
#define SENSOR_NORMALIZATION_H

#include <math.h>

namespace SensorNormalization {

inline bool normalizeTemperature(float& value)
{
    if (!isfinite(value))
        return false;
    if (value < -40.0f)
        value = -40.0f;
    if (value > 100.0f)
        value = 100.0f;
    return true;
}

inline bool normalizeHumidity(float& value)
{
    if (!isfinite(value) || value < 0.0f)
        return false;
    if (value > 100.0f)
        value = 100.0f;
    return true;
}

inline bool normalizeLight(float& value)
{
    if (!isfinite(value) || value < 0.0f)
        return false;
    if (value > 65535.0f)
        value = 65535.0f;
    return true;
}

inline float sanitizeHumidityOrZero(float value)
{
    return normalizeHumidity(value) ? value : 0.0f;
}

inline float sanitizeLightOrZero(float value)
{
    return normalizeLight(value) ? value : 0.0f;
}

inline float sanitizeTemperatureOr(float value, float fallback)
{
    return normalizeTemperature(value) ? value : fallback;
}

} // namespace SensorNormalization

#endif // SENSOR_NORMALIZATION_H
