#include "SensorDataManager.h"
#include <SD.h>
#include "config.h"
#include "SensorNormalization.h"
#include "ThresholdValidation.h"

#define DEBUG_PRINTLN(level, ...)        \
    do                                   \
    {                                    \
        if (DEBUG_LEVEL >= level)        \
        {                                \
            Serial.println(__VA_ARGS__); \
        }                                \
    } while (0)

void SensorDataManager::begin()
{
    loadLocalThresholdsFromNVS();
    loadCloudThresholdsFromNVS();

    // Load data node terakhir yang tersimpan sebelum reboot
    loadNodesFromNVS();
    DEBUG_PRINTLN(3, "[DATA] Node states & Mode loaded from NVS.");
    recalculateLocalAverage();
}

void SensorDataManager::setSourceMode(DataSourceMode mode)
{
    currentMode = mode;
    runtimeFallbackToLocal = false;
    
    // Simpan ke NVS
    prefs.begin("thresholds", false);
    prefs.putInt("src_mode", (int)mode);
    prefs.end();

    if (mode != SOURCE_AUTO)
    {
        consecutiveCloudFailures = 0;
        consecutiveCloudSuccesses = 0;
    }
    
    if (mode == SOURCE_LOCAL)
    {
        recalculateLocalAverage();
    }
    refreshDisplayData();
}

// Load semua node dari NVS saat startup
void SensorDataManager::loadNodesFromNVS()
{
    prefs.begin("nodes_bkp", true); // Read-only
    for (int i = 0; i < MAX_NODES; i++) 
    {
        char k[10];
        snprintf(k, sizeof(k), "n%d", i);
        if (prefs.isKey(k))
        {
            prefs.getBytes(k, &nodes[i], sizeof(NodeDataEntry));
            nodes[i].temp = SensorNormalization::sanitizeTemperatureOr(nodes[i].temp, 0.0f);
            nodes[i].hum = SensorNormalization::sanitizeHumidityOrZero(nodes[i].hum);
            nodes[i].lux = SensorNormalization::sanitizeLightOrZero(nodes[i].lux);
            // Cache node hasil restore tidak dianggap live sampai ada packet baru.
            nodes[i].active = false;
            nodes[i].lastMillis = 0;
        }
    }
    prefs.end();
    recalculateLocalAverage(); // Hitung rata-rata awal dari data backup
}

// Simpan satu node ke NVS saat update
void SensorDataManager::saveNodeToNVS(int idx)
{
    if (idx < 0 || idx >= MAX_NODES)
        return;
    prefs.begin("nodes_bkp", false); // Read-Write
    char k[10];
    snprintf(k, sizeof(k), "n%d", idx);
    prefs.putBytes(k, &nodes[idx], sizeof(NodeDataEntry));
    prefs.end();
}

void SensorDataManager::refreshDisplayData() {
    const bool useLocal = isUsingLocalData();

    if (useLocal) {
        // Tampilkan Data dari Buffer LOKAL
        temperature = _lTemp;
        humidity = _lHum;
        light = _lLight; // Opsional
        isFoggy = _lFog;
    } else {
        // Tampilkan Data dari Buffer CLOUD
        temperature = _cTemp;
        humidity = _cHum;
        light = _cLight;
        isFoggy = _cFog;
    }
}

bool SensorDataManager::setLocalThresholds(float tMn, float tMx, float hMn, float hMx)
{
    if (!ThresholdValidation::isTemperatureRangeValid(tMn, tMx) ||
        !ThresholdValidation::isHumidityRangeValid(hMn, hMx))
        return false;
    _localThresholds.tempMin = tMn;
    _localThresholds.tempMax = tMx;
    _localThresholds.humMin = hMn;
    _localThresholds.humMax = hMx;
    _localThresholds.valid = true;
    saveLocalThresholdsToNVS();
    return true;
}

void SensorDataManager::updateThresholds(float tMin, float tMax, float hMin, float hMax, float lMin, float lMax)
{
    bool tempUpdated = false;
    bool humUpdated = false;
    bool lightUpdated = false;
    if (ThresholdValidation::isTemperatureRangeValid(tMin, tMax))
    {
        _cloudThresholds.tempMin = tMin;
        _cloudThresholds.tempMax = tMax;
        tempUpdated = true;
    }
    if (ThresholdValidation::isHumidityRangeValid(hMin, hMax))
    {
        _cloudThresholds.humMin = hMin;
        _cloudThresholds.humMax = hMax;
        humUpdated = true;
    }
    if (ThresholdValidation::isLightRangeValid(lMin, lMax))
    {
        _cloudThresholds.lightMin = lMin;
        _cloudThresholds.lightMax = lMax;
        lightUpdated = true;
    }
    if (tempUpdated && humUpdated)
    {
        _cloudThresholds.valid = true;
        lastCloudThresholdSuccessMs = millis();
        saveCloudThresholdsToNVS();
    }
    else if (lightUpdated)
    {
        saveCloudThresholdsToNVS();
    }
}

void SensorDataManager::updateFromCloud(float temp, float hum, float lgt)
{
    updateFromCloudPartial(true, temp, true, hum, true, lgt);
}

void SensorDataManager::updateFromCloudPartial(bool hasTemp, float temp, bool hasHum, float hum, bool hasLgt, float lgt)
{
    // Simpan ke Buffer Cloud (_c...)
    if (hasTemp && SensorNormalization::normalizeTemperature(temp))
        _cTemp = temp;
    if (hasHum && SensorNormalization::normalizeHumidity(hum))
        _cHum = hum;
    if (hasLgt && SensorNormalization::normalizeLight(lgt))
        _cLight = lgt;

    // Update tampilan sesuai mode saat ini
    refreshDisplayData();
}

void SensorDataManager::updateFromCloudFull(float temp, float hum, float lgt, bool fogStatus)
{
    if (SensorNormalization::normalizeTemperature(temp))
        _cTemp = temp;
    if (SensorNormalization::normalizeHumidity(hum))
        _cHum = hum;
    if (SensorNormalization::normalizeLight(lgt))
        _cLight = lgt;
    _cFog = fogStatus;
    lastCloudFogSuccessMs = millis();

    refreshDisplayData();
}

// [UPDATE] Menyimpan TX, RX, Size, dan backup ke NVS
void SensorDataManager::updateFromNode(const char *nodeName, float temp, float hum, float lgt, bool foggy, float conf, unsigned long tx, unsigned long rx, size_t sz)
{
    int idx = -1;

    // Logika Mapping ID
    if (strcmp(nodeName, "cam-1") == 0)
        idx = 10;
    else if (strcmp(nodeName, "cam-2") == 0)
        idx = 11;
    else
    {
        // node-1 s/d node-10
        String n = String(nodeName);
        if (n.startsWith("node-"))
        {
            idx = n.substring(5).toInt() - 1;
        }
    }

    if (idx < 0 || idx >= MAX_NODES)
        return;

    strncpy(nodes[idx].node_name, nodeName, 9);
    if (SensorNormalization::normalizeTemperature(temp))
        nodes[idx].temp = temp;
    if (SensorNormalization::normalizeHumidity(hum))
        nodes[idx].hum = hum;
    if (SensorNormalization::normalizeLight(lgt))
        nodes[idx].lux = lgt;
    nodes[idx].is_foggy = foggy;
    nodes[idx].confidence = conf;
    nodes[idx].lastMillis = millis();
    nodes[idx].txEpoch = tx;
    nodes[idx].rxEpoch = rx;
    nodes[idx].payloadSize = sz;
    nodes[idx].active = true;

    saveNodeToNVS(idx);

    // Hanya hitung rata-rata jika yang update adalah node sensor (0-9)
   recalculateLocalAverage();
}

void SensorDataManager::recalculateLocalAverage()
{
    float sumT = 0, sumH = 0, sumL = 0;
    int sensorCount = 0;
    bool cameraFogStatus = false; // Variabel baru untuk menampung status kamera
    unsigned long now = millis();
    const unsigned long MAX_AGE = LOCAL_DATA_VALID_MS; 

    for (int i = 0; i < MAX_NODES; i++) 
    {
        if (nodes[i].active && (now - nodes[i].lastMillis < MAX_AGE))
        {
            // Jika ini node sensor biasa (node-1 s/d node-10)
            if (i < 10) {
                float temp = nodes[i].temp;
                float hum = nodes[i].hum;
                float light = nodes[i].lux;
                if (SensorNormalization::normalizeTemperature(temp) &&
                    SensorNormalization::normalizeHumidity(hum) &&
                    SensorNormalization::normalizeLight(light)) {
                    sumT += temp;
                    sumH += hum;
                    sumL += light;
                    sensorCount++;
                }
            }
            
            // Jika ini node kamera (cam-1 atau cam-2)
            // Index 10 = cam-1, Index 11 = cam-2
            if (i >= 10) {
                if (nodes[i].is_foggy) {
                    cameraFogStatus = true; // Jika salah satu kamera berkabut, set true
                }
            }
        }
    }

    if (sensorCount > 0) {
        _lTemp = sumT / sensorCount;
        _lHum = sumH / sensorCount;
        _lLight = sumL / sensorCount;
    } else {
        _lTemp = -99.9f;
        _lHum = -1.0f;
        _lLight = -1.0f;
    }
    
    // UPDATE variabel kabut lokal agar muncul di layar LCD dan Dashboard
    _lFog = cameraFogStatus; 

    refreshDisplayData();
}

String SensorDataManager::getModeString() const
{
    switch (currentMode)
    {
    case SOURCE_CLOUD:
        return "CLOUD";
    case SOURCE_LOCAL:
        return "LOCAL (AVG)";
    case SOURCE_AUTO:
        return isUsingLocalData() ? "AUTO (LOCAL)" : "AUTO (CLOUD)";
    default:
        return "UNKNOWN";
    }
}

String SensorDataManager::getConfiguredModeString() const
{
    switch (currentMode)
    {
    case SOURCE_CLOUD:
        return "CLOUD";
    case SOURCE_LOCAL:
        return "LOCAL";
    case SOURCE_AUTO:
        return "AUTO";
    default:
        return "UNKNOWN";
    }
}

bool SensorDataManager::canEditLocalThresholds() const
{
    return true;
}

bool SensorDataManager::canEditLocalSchedules() const
{
    return true;
}

const ThresholdProfile& SensorDataManager::getRuntimeThresholdProfile() const
{
    return activeThresholdProfile();
}

const ThresholdProfile& SensorDataManager::getLocalThresholdProfile() const
{
    return _localThresholds;
}

const ThresholdProfile& SensorDataManager::getCloudThresholdProfile() const
{
    return _cloudThresholds;
}

float SensorDataManager::getTempMin() const
{
    return activeThresholdProfile().tempMin;
}

float SensorDataManager::getTempMax() const
{
    return activeThresholdProfile().tempMax;
}

float SensorDataManager::getHumMin() const
{
    return activeThresholdProfile().humMin;
}

float SensorDataManager::getHumMax() const
{
    return activeThresholdProfile().humMax;
}

float SensorDataManager::getLightMin() const
{
    return activeThresholdProfile().lightMin;
}

float SensorDataManager::getLightMax() const
{
    return activeThresholdProfile().lightMax;
}

void SensorDataManager::setCloudFog(bool status)
{
    _cFog = status;
    lastCloudFogSuccessMs = millis();
    refreshDisplayData();
}

bool SensorDataManager::hasCloudThresholdProfile() const
{
    return _cloudThresholds.valid;
}

bool SensorDataManager::hasFreshCloudThresholdProfile(unsigned long now) const
{
    if (!_cloudThresholds.valid || lastCloudThresholdSuccessMs == 0)
        return false;
    return (now - lastCloudThresholdSuccessMs) <= THRESHOLD_STALE_THRESHOLD_MS;
}

bool SensorDataManager::hasFreshCloudSensorData(unsigned long now) const
{
    if (lastCloudSensorSuccessMs == 0)
        return false;
    return (now - lastCloudSensorSuccessMs) <= CLOUD_CONTROL_DATA_VALID_MS;
}

bool SensorDataManager::hasFreshCloudFogData(unsigned long now) const
{
    if (lastCloudFogSuccessMs == 0)
        return false;
    return (now - lastCloudFogSuccessMs) <= CLOUD_CONTROL_DATA_VALID_MS;
}

bool SensorDataManager::hasUsableLocalFogData() const
{
    return countCameraNodesWithinAge(LOCAL_DATA_VALID_MS) > 0;
}

void SensorDataManager::noteCloudSensorSuccess(unsigned long nowMs)
{
    lastCloudSensorSuccessMs = nowMs;
}

void SensorDataManager::noteLocalSensorUpdate(unsigned long nowMs)
{
    lastLocalSensorUpdateMs = nowMs;
}

unsigned long SensorDataManager::getLastCloudSensorSuccessMs() const
{
    return lastCloudSensorSuccessMs;
}

unsigned long SensorDataManager::getLastLocalSensorUpdateMs() const
{
    return lastLocalSensorUpdateMs;
}

void SensorDataManager::onCloudSuccess()
{
    if (currentMode != SOURCE_AUTO)
        return;

    consecutiveCloudFailures = 0;
    consecutiveCloudSuccesses++;

    refreshDisplayData();
}

void SensorDataManager::onCloudFailure()
{
    if (currentMode != SOURCE_AUTO) return;
    consecutiveCloudFailures++;
    consecutiveCloudSuccesses = 0;
}

bool SensorDataManager::isUsingLocalData() const
{
    if (currentMode == SOURCE_LOCAL)
        return true;
    if (runtimeFallbackToLocal)
        return true;
    return false;
}

void SensorDataManager::setRuntimeFallbackToLocal(bool enabled)
{
    if (runtimeFallbackToLocal == enabled)
        return;
    runtimeFallbackToLocal = enabled;
    refreshDisplayData();
}

const ThresholdProfile& SensorDataManager::activeThresholdProfile() const
{
    if (isUsingLocalData())
        return _localThresholds;
    return _cloudThresholds;
}

void SensorDataManager::loadLocalThresholdsFromNVS()
{
    prefs.begin("thresholds", true);
    _localThresholds.valid = true;
    if (prefs.isKey("tm_min")) _localThresholds.tempMin = prefs.getFloat("tm_min", _localThresholds.tempMin);
    if (prefs.isKey("tm_max")) _localThresholds.tempMax = prefs.getFloat("tm_max", _localThresholds.tempMax);
    if (prefs.isKey("hm_min")) _localThresholds.humMin = prefs.getFloat("hm_min", _localThresholds.humMin);
    if (prefs.isKey("hm_max")) _localThresholds.humMax = prefs.getFloat("hm_max", _localThresholds.humMax);
    if (prefs.isKey("lm_min")) _localThresholds.lightMin = prefs.getFloat("lm_min", _localThresholds.lightMin);
    if (prefs.isKey("lm_max")) _localThresholds.lightMax = prefs.getFloat("lm_max", _localThresholds.lightMax);

    // Load Mode Terakhir (Default ke AUTO jika belum ada)
    currentMode = (DataSourceMode)prefs.getInt("src_mode", (int)SOURCE_AUTO);
    prefs.end();
}

void SensorDataManager::saveLocalThresholdsToNVS()
{
    prefs.begin("thresholds", false);
    prefs.putFloat("tm_min", _localThresholds.tempMin);
    prefs.putFloat("tm_max", _localThresholds.tempMax);
    prefs.putFloat("hm_min", _localThresholds.humMin);
    prefs.putFloat("hm_max", _localThresholds.humMax);
    prefs.putFloat("lm_min", _localThresholds.lightMin);
    prefs.putFloat("lm_max", _localThresholds.lightMax);
    prefs.end();
}

void SensorDataManager::loadCloudThresholdsFromNVS()
{
    lastCloudThresholdSuccessMs = 0;
    prefs.begin("thresholds_cld", true);
    _cloudThresholds.valid = prefs.getBool("valid", false);
    if (_cloudThresholds.valid)
    {
        _cloudThresholds.tempMin = prefs.getFloat("tm_min", _cloudThresholds.tempMin);
        _cloudThresholds.tempMax = prefs.getFloat("tm_max", _cloudThresholds.tempMax);
        _cloudThresholds.humMin = prefs.getFloat("hm_min", _cloudThresholds.humMin);
        _cloudThresholds.humMax = prefs.getFloat("hm_max", _cloudThresholds.humMax);
        _cloudThresholds.lightMin = prefs.getFloat("lm_min", _cloudThresholds.lightMin);
        _cloudThresholds.lightMax = prefs.getFloat("lm_max", _cloudThresholds.lightMax);
    }
    prefs.end();
}

void SensorDataManager::saveCloudThresholdsToNVS()
{
    prefs.begin("thresholds_cld", false);
    prefs.putFloat("tm_min", _cloudThresholds.tempMin);
    prefs.putFloat("tm_max", _cloudThresholds.tempMax);
    prefs.putFloat("hm_min", _cloudThresholds.humMin);
    prefs.putFloat("hm_max", _cloudThresholds.humMax);
    prefs.putFloat("lm_min", _cloudThresholds.lightMin);
    prefs.putFloat("lm_max", _cloudThresholds.lightMax);
    prefs.putBool("valid", _cloudThresholds.valid);
    prefs.end();
}

int SensorDataManager::countNodesWithinAge(unsigned long maxAgeMs) const
{
    int count = 0;
    const unsigned long now = millis();
    for (int i = 0; i < MAX_NODES; i++)
    {
        if (nodes[i].active && (now - nodes[i].lastMillis < maxAgeMs))
            count++;
    }
    return count;
}

int SensorDataManager::countSensorNodesWithinAge(unsigned long maxAgeMs) const
{
    int count = 0;
    const unsigned long now = millis();
    for (int i = 0; i < 10; i++)
    {
        if (nodes[i].active && (now - nodes[i].lastMillis < maxAgeMs))
            count++;
    }
    return count;
}

int SensorDataManager::countCameraNodesWithinAge(unsigned long maxAgeMs) const
{
    int count = 0;
    const unsigned long now = millis();
    for (int i = 10; i < MAX_NODES; i++)
    {
        if (nodes[i].active && (now - nodes[i].lastMillis < maxAgeMs))
            count++;
    }
    return count;
}

int SensorDataManager::getActiveNodeCount()
{
    return countNodesWithinAge(NODE_TIMEOUT_MS);
}

int SensorDataManager::getUsableLocalNodeCount() const
{
    return countSensorNodesWithinAge(LOCAL_DATA_VALID_MS);
}

bool SensorDataManager::loadFromLog()
{
    // Fungsi legacy untuk memuat data rata-rata global dari log.csv
    // Data individual node sekarang ditangani oleh loadNodesFromNVS
    // Kita tetap pertahankan untuk backward compatibility data utama.
    File f = SD.open("/log.csv", FILE_READ);
    if (!f)
        return false;
    if (!f.size())
    {
        f.close();
        return false;
    }

    String lastLine = "";
    long pos = f.size() - 1;
    int chars_read = 0;

    while (pos > 0 && chars_read < 512)
    {
        f.seek(pos);
        char c = f.read();
        if (c == '\n' && lastLine.length() > 0)
            break;
        if (c != '\r')
            lastLine += c;
        pos--;
        chars_read++;
    }

    if (pos == 0 && lastLine.length() == 0 && f.available())
    {
        f.seek(0);
        lastLine = f.readStringUntil('\n');
        lastLine.trim();
    }
    else
    {
        String temp = "";
        for (int i = lastLine.length() - 1; i >= 0; i--)
            temp += lastLine.charAt(i);
        lastLine = temp;
    }
    f.close();

    if (lastLine.length() == 0 || lastLine.startsWith(F("DateTime,")))
        return false;

    // Parsing CSV sederhana (support legacy 14 kolom & format baru 27 kolom)
    const int kMaxCols = 32;
    int commas[kMaxCols];
    int commaCount = 0;
    for (size_t i = 0; i < lastLine.length() && commaCount < kMaxCols; ++i)
    {
        if (lastLine[i] == ',')
            commas[commaCount++] = (int)i;
    }
    const int colCount = commaCount + 1;
    if (colCount < 14)
        return false;

    auto getField = [&](int col) -> String {
        if (col < 0 || col >= colCount) return String();
        int start = (col == 0) ? 0 : (commas[col - 1] + 1);
        int end = (col < commaCount) ? commas[col] : (int)lastLine.length();
        return lastLine.substring(start, end);
    };

    const int tCol = 1;
    const int hCol = 2;
    const int lCol = 3;
    int tMinCol = -1, tMaxCol = -1, hMinCol = -1, hMaxCol = -1;

    if (colCount >= 27)
    {
        tMinCol = 11; tMaxCol = 12; hMinCol = 13; hMaxCol = 14;
    }
    else
    {
        tMinCol = 10; tMaxCol = 11; hMinCol = 12; hMaxCol = 13;
    }

    float t = getField(tCol).toFloat();
    float h = getField(hCol).toFloat();
    float l = getField(lCol).toFloat();
    float tMn = getField(tMinCol).toFloat();
    float tMx = getField(tMaxCol).toFloat();
    float hMn = getField(hMinCol).toFloat();
    float hMx = getField(hMaxCol).toFloat();

    if (tMn < 80.0f && tMx > -20.0f && tMn <= tMx &&
        hMn >= 0.0f && hMx <= 100.0f && hMn <= hMx)
    {
        _cloudThresholds.tempMin = tMn;
        _cloudThresholds.tempMax = tMx;
        _cloudThresholds.humMin = hMn;
        _cloudThresholds.humMax = hMx;
        _cloudThresholds.lightMin = 10750.0f;
        _cloudThresholds.lightMax = 21500.0f;
        _cloudThresholds.valid = true;
    }
    updateFromCloud(t, h, l);
    return true;
}
