#ifndef SENSOR_DATA_MANAGER_H
#define SENSOR_DATA_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

#define MAX_NODES 12
#define NODE_TIMEOUT_MS 300000UL
#define LOCAL_DATA_VALID_MS NODE_TIMEOUT_MS
#define AUTO_CLOUD_FAILURE_THRESHOLD 2
#define AUTO_CLOUD_RECOVERY_THRESHOLD 3
#define AUTO_CLOUD_RETRY_INTERVAL_MS 60000UL // Probe cloud tiap 1 menit saat AUTO sedang pakai lokal

enum DataSourceMode
{
    SOURCE_CLOUD,
    SOURCE_LOCAL,
    SOURCE_AUTO
};

struct ThresholdProfile
{
    float tempMin = 23.0f;
    float tempMax = 29.0f;
    float humMin = 58.0f;
    float humMax = 78.0f;
    float lightMin = 10750.0f;
    float lightMax = 21500.0f;
    bool valid = false;
};

struct NodeDataEntry
{
    char node_name[10]; // "node-1" atau "cam-1"
    float temp;
    float hum;
    float lux;
    bool is_foggy;    // Khusus Kamera
    float confidence; // Khusus Kamera
    unsigned long lastMillis;
    unsigned long txEpoch;
    unsigned long rxEpoch;
    size_t payloadSize;
    bool active;
};

class SensorDataManager
{
public:
    float temperature = -99.9, humidity = -1.0, light = -1.0;
    bool isFoggy = false;
    DataSourceMode currentMode = SOURCE_AUTO;

    void begin();
    bool setLocalThresholds(float tMn, float tMx, float hMn, float hMx);
    void updateThresholds(float tMin, float tMax, float hMin, float hMax, float lMin, float lMax);
    void updateFromCloud(float temp, float hum, float lgt);
    void updateFromCloudPartial(bool hasTemp, float temp, bool hasHum, float hum, bool hasLgt, float lgt);

    // [UPDATE] Tambah parameter tx, rx, size
    void updateFromCloudFull(float temp, float hum, float lgt, bool fogStatus); 

    void updateFromNode(const char *nodeName, float temp, float hum, float lgt, bool foggy, float conf, unsigned long tx, unsigned long rx, size_t sz);

    void recalculateLocalAverage();
    bool loadFromLog();
    void setSourceMode(DataSourceMode mode);
    String getModeString() const;
    String getConfiguredModeString() const;
    bool canEditLocalThresholds() const;
    bool canEditLocalSchedules() const;

    const ThresholdProfile& getRuntimeThresholdProfile() const;
    const ThresholdProfile& getLocalThresholdProfile() const;
    const ThresholdProfile& getCloudThresholdProfile() const;
    float getTempMin() const;
    float getTempMax() const;
    float getHumMin() const;
    float getHumMax() const;
    float getLightMin() const;
    float getLightMax() const;
    bool hasCloudThresholdProfile() const;
    bool hasFreshCloudThresholdProfile(unsigned long now) const;
    bool hasFreshCloudSensorData(unsigned long now) const;
    void setCloudFog(bool status);
    bool hasFreshCloudFogData(unsigned long now) const;
    bool hasUsableLocalFogData() const;
    void noteCloudSensorSuccess(unsigned long nowMs);
    void noteLocalSensorUpdate(unsigned long nowMs);
    unsigned long getLastCloudSensorSuccessMs() const;
    unsigned long getLastLocalSensorUpdateMs() const;

    void onCloudSuccess();
    void onCloudFailure();
    bool isUsingLocalData() const;
    void setRuntimeFallbackToLocal(bool enabled);
    int getActiveNodeCount();
    int getUsableLocalNodeCount() const;
    uint32_t getCloudFailureCount() const { return consecutiveCloudFailures; }
    uint32_t getCloudSuccessCount() const { return consecutiveCloudSuccesses; }
    const NodeDataEntry *getAllNodes() const { return nodes; }
    
    // Fungsi baru untuk memperbarui tampilan berdasarkan mode
    void refreshDisplayData(); 

private:
    NodeDataEntry nodes[MAX_NODES];
    uint32_t consecutiveCloudFailures = 0;
    uint32_t consecutiveCloudSuccesses = 0;
    Preferences prefs;
    ThresholdProfile _localThresholds;
    ThresholdProfile _cloudThresholds;
    unsigned long lastCloudSensorSuccessMs = 0;
    unsigned long lastLocalSensorUpdateMs = 0;
    unsigned long lastCloudThresholdSuccessMs = 0;
    unsigned long lastCloudFogSuccessMs = 0;

    // [BARU] Variabel Buffer untuk memisahkan sumber data
    float _cTemp = 0, _cHum = 0, _cLight = 0; // Cloud Buffer
    bool _cFog = false;
    
    float _lTemp = 0, _lHum = 0, _lLight = 0; // Local Buffer
    bool _lFog = false;
    bool runtimeFallbackToLocal = false;

    const ThresholdProfile& activeThresholdProfile() const;
    void loadLocalThresholdsFromNVS();
    void saveLocalThresholdsToNVS();
    void loadCloudThresholdsFromNVS();
    void saveCloudThresholdsToNVS();
    void saveNodeToNVS(int idx);
    void loadNodesFromNVS();
    int countNodesWithinAge(unsigned long maxAgeMs) const;
    int countSensorNodesWithinAge(unsigned long maxAgeMs) const;
    int countCameraNodesWithinAge(unsigned long maxAgeMs) const;
};

#endif
