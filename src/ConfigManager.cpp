///////////////////////////////////////////////////////////////////////////////////
// File: ConfigManager.cpp (REVISED)
//
// Description: Mengelola semua aspek konfigurasi perangkat dengan ESPAsyncWebServer.
//              - [FIX] Menambahkan kata sandi ke Access Point portal konfigurasi.
///////////////////////////////////////////////////////////////////////////////////

#include "ConfigManager.h"
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "CryptoUtils.h"
#include "LCDDisplay.h"
#include "PortalAssets.h"
#include <ArduinoJson.h>

namespace
{
constexpr unsigned long ADMIN_SESSION_TTL_MS = 30UL * 60UL * 1000UL;
constexpr size_t ADMIN_AUTH_RATE_SLOT_COUNT = 8;
constexpr uint8_t ADMIN_AUTH_MAX_FAILURES = 5;
constexpr unsigned long ADMIN_AUTH_BLOCK_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long ADMIN_AUTH_WINDOW_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t ADMIN_PROOF_MAX_AGE_SEC = CryptoUtils::ENCRYPTED_REPLAY_WINDOW_SOFT_SEC;
String g_adminSessionToken;
unsigned long g_adminSessionIssuedMs = 0;
String g_adminSessionBinding;

inline int sha256StartsCompat(mbedtls_sha256_context* ctx, int is224)
{
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    return mbedtls_sha256_starts(ctx, is224);
#else
    return mbedtls_sha256_starts_ret(ctx, is224);
#endif
}

inline int sha256UpdateCompat(mbedtls_sha256_context* ctx, const unsigned char* input, size_t ilen)
{
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    return mbedtls_sha256_update(ctx, input, ilen);
#else
    return mbedtls_sha256_update_ret(ctx, input, ilen);
#endif
}

inline int sha256FinishCompat(mbedtls_sha256_context* ctx, unsigned char output[32])
{
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
    return mbedtls_sha256_finish(ctx, output);
#else
    return mbedtls_sha256_finish_ret(ctx, output);
#endif
}

struct AdminAuthRateSlot
{
    String binding;
    uint8_t failures = 0;
    unsigned long firstFailureMs = 0;
    bool blocked = false;
    unsigned long blockedAtMs = 0;
};

AdminAuthRateSlot g_adminAuthRateSlots[ADMIN_AUTH_RATE_SLOT_COUNT];

struct OtaUploadAuthContext
{
    bool proofAuthorized = false;
    bool uploadRejected = false;
    String expectedFilename;
    String expectedSha256;
    size_t expectedSize = 0;
    size_t bytesReceived = 0;
    bool metadataValid = false;
    bool hashActive = false;
    mbedtls_sha256_context shaCtx;

    OtaUploadAuthContext()
    {
        mbedtls_sha256_init(&shaCtx);
    }

    ~OtaUploadAuthContext()
    {
        mbedtls_sha256_free(&shaCtx);
    }
};

String generateAdminSessionToken()
{
    char token[33];
    for (size_t i = 0; i < 16; ++i)
    {
        const uint8_t v = static_cast<uint8_t>(esp_random() & 0xFF);
        snprintf(token + (i * 2), 3, "%02x", v);
    }
    token[32] = '\0';
    return String(token);
}

String normalizeBinding(const String& binding)
{
    if (binding.length() == 0)
        return "unknown";
    return binding;
}

void clearAuthRateSlot(AdminAuthRateSlot& slot)
{
    slot.binding = "";
    slot.failures = 0;
    slot.firstFailureMs = 0;
    slot.blocked = false;
    slot.blockedAtMs = 0;
}

AdminAuthRateSlot* findAuthRateSlot(const String& binding)
{
    const String normalized = normalizeBinding(binding);
    AdminAuthRateSlot* emptySlot = nullptr;
    AdminAuthRateSlot* reusableSlot = nullptr;
    const unsigned long now = millis();

    for (auto& slot : g_adminAuthRateSlots)
    {
        if (slot.binding == normalized)
            return &slot;

        if (slot.binding.length() == 0 && !emptySlot)
            emptySlot = &slot;

        const bool blockExpired = slot.blocked && (now - slot.blockedAtMs) > ADMIN_AUTH_BLOCK_MS;
        const bool windowExpired = !slot.blocked && slot.firstFailureMs != 0 && (now - slot.firstFailureMs) > ADMIN_AUTH_WINDOW_MS;
        if ((blockExpired || windowExpired) && !reusableSlot)
            reusableSlot = &slot;
    }

    if (emptySlot)
        return emptySlot;
    if (reusableSlot)
    {
        clearAuthRateSlot(*reusableSlot);
        return reusableSlot;
    }
    clearAuthRateSlot(g_adminAuthRateSlots[0]);
    return &g_adminAuthRateSlots[0];
}

String decryptAdminProofPayload(AsyncWebServerRequest* request)
{
    if (!request || !request->hasHeader("X-Admin-Proof"))
        return "";

    const String proof = request->getHeader("X-Admin-Proof")->value();
    if (proof.length() == 0)
        return "";

    char decrypted[CryptoUtils::MAX_DECRYPTED_SIZE];
    size_t decryptedLen = 0;
    if (!CryptoUtils::decryptNodeMediniPayload(proof.c_str(),
                                               proof.length(),
                                               decrypted,
                                               sizeof(decrypted),
                                               &decryptedLen,
                                               nullptr,
                                               ADMIN_PROOF_MAX_AGE_SEC))
    {
        return "";
    }
    return String(decrypted, decryptedLen);
}

String normalizeHexLower(String value)
{
    value.trim();
    value.toLowerCase();
    return value;
}

String sha256ToHex(const unsigned char digest[32])
{
    char hex[65];
    for (size_t i = 0; i < 32; ++i)
        snprintf(hex + (i * 2), 3, "%02x", digest[i]);
    hex[64] = '\0';
    return String(hex);
}

bool parseAdminProofDocument(AsyncWebServerRequest* request, JsonDocument& proofDoc)
{
    const String payload = decryptAdminProofPayload(request);
    if (payload.length() == 0)
        return false;

    if (!deserializeJson(proofDoc, payload))
    {
        String method = proofDoc["method"] | "";
        String path = proofDoc["path"] | "";
        method.trim();
        path.trim();
        if (!request)
            return false;
        if (!method.equalsIgnoreCase(request->methodToString()))
            return false;
        if (path != request->url())
            return false;
        const char* token = proofDoc["token"] | "";
        return token && token[0] != '\0';
    }

    const int firstSep = payload.indexOf('\n');
    const int secondSep = payload.indexOf('\n', firstSep + 1);
    if (firstSep <= 0 || secondSep <= firstSep)
        return false;

    proofDoc["token"] = payload.substring(0, firstSep);
    String method = payload.substring(firstSep + 1, secondSep);
    String path = payload.substring(secondSep + 1);
    method.trim();
    path.trim();
    proofDoc["method"] = method;
    proofDoc["path"] = path;

    if (!request)
        return false;
    if (!method.equalsIgnoreCase(request->methodToString()))
        return false;
    if (path != request->url())
        return false;
    return true;
}

String extractAdminTokenFromProof(AsyncWebServerRequest* request)
{
    JsonDocument proofDoc;
    if (!parseAdminProofDocument(request, proofDoc))
        return "";
    return proofDoc["token"] | "";
}

bool validateRequestProofScope(AsyncWebServerRequest* request, JsonDocument& proofDoc)
{
    if (!request)
        return false;

    if (request->url() == "/download")
    {
        if (!request->hasParam("file"))
            return false;
        const String requestedFile = request->getParam("file")->value();
        const String proofFile = proofDoc["file"] | "";
        return requestedFile.length() > 0 && proofFile == requestedFile;
    }

    if (request->url() == "/doUpdate")
    {
        const String proofFilename = proofDoc["filename"] | "";
        const String proofSha = normalizeHexLower(proofDoc["sha256"] | "");
        const size_t proofSize = proofDoc["size"] | 0;
        if (proofFilename.length() == 0 || proofSha.length() != 64 || proofSize == 0)
            return false;
        if (!request->hasHeader("X-Upload-Filename") ||
            !request->hasHeader("X-Upload-Size") ||
            !request->hasHeader("X-Upload-SHA256"))
        {
            return false;
        }

        const String headerFilename = request->getHeader("X-Upload-Filename")->value();
        const String headerSize = request->getHeader("X-Upload-Size")->value();
        const String headerSha = normalizeHexLower(request->getHeader("X-Upload-SHA256")->value());
        if (headerFilename != proofFilename)
            return false;
        if (headerSha != proofSha)
            return false;
        if (headerSize.toInt() <= 0 || static_cast<size_t>(headerSize.toInt()) != proofSize)
            return false;
        return true;
    }

    return true;
}

OtaUploadAuthContext* ensureOtaUploadAuthContext(AsyncWebServerRequest* request)
{
    if (!request)
        return nullptr;
    auto* ctx = static_cast<OtaUploadAuthContext*>(request->_tempObject);
    if (ctx)
        return ctx;
    ctx = new (std::nothrow) OtaUploadAuthContext();
    if (!ctx)
        return nullptr;
    request->_tempObject = ctx;
    return ctx;
}

void clearOtaUploadAuthContext(AsyncWebServerRequest* request)
{
    if (!request)
        return;
    auto* ctx = static_cast<OtaUploadAuthContext*>(request->_tempObject);
    if (ctx)
        delete ctx;
    request->_tempObject = nullptr;
}
} // namespace

// ==================================================================================
//   Definisi Variabel Konfigurasi Global
// ==================================================================================
char active_ssid[64];
char active_pwd[64];
char active_api_token[128];
char active_ta_token[128];
char active_th_url[128];
char active_nd_url[128];
char active_nd_url_base[128];
char active_device_status_post_url[128];
char active_device_status_get_url[128];
char active_admin_pass[64];
bool gprs_enabled = false;

// ==================================================================================
//   HTML Content
// ==================================================================================
// CONFIG_PAGE has been moved to PortalAssets.h


const char OTA_UPDATE_PAGE[] PROGMEM = R"=====(
<!DOCTYPE html><html><head><title>ESP32 Firmware Update</title><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;background-color:#f4f4f4;padding:10px;color:#333;}h1{text-align:center;color:#b30000;}div{background-color:#fff;max-width:500px;margin:0 auto;padding:20px;border-radius:8px;box-shadow:0 2px 5px rgba(0,0,0,0.1);}input[type='file']{border:1px solid #ccc;display:inline-block;padding:6px 12px;cursor:pointer;}input[type='submit']{background-color:#d9534f;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;font-size:1em;}input[type='submit']:hover{background-color:#c9302c;}#prg{width:100%;background-color:#ddd;margin-top:10px;}#bar{width:0%;height:30px;background-color:#4CAF50;text-align:center;line-height:30px;color:white;}</style></head><body><div><h1>Firmware Update</h1><p>Select a <code>firmware.bin</code> file to upload.</p><p>The device will restart after a successful update.</p><form method='POST' action='/doUpdate' enctype='multipart/form-data' id='upload_form'><input type='file' name='update' id='file' accept='.bin'><input type='submit' value='Update Firmware'></form><div id='prg' style='display:none;'><div id='bar'>0%</div></div></div><script>function el(id){return document.getElementById(id);}el('upload_form').addEventListener('submit',function(e){e.preventDefault();var f=el('file').files[0];if(!f){alert('Please select a file!');return;}el('prg').style.display='block';var xhr=new XMLHttpRequest();xhr.open('POST','/doUpdate');xhr.upload.addEventListener('progress',function(e){if(e.lengthComputable){var p=Math.round((e.loaded/e.total)*100);el('bar').style.width=p+'%';el('bar').innerHTML=p+'%';}});xhr.onload=function(){if(xhr.status===200){alert('Update success! Device is rebooting...');window.location.href='/';}else{alert('Update failed! Error: '+xhr.statusText);}};xhr.onerror=function(){alert('Update failed! Check connection.');};var d=new FormData();d.append('update',f,f.name);xhr.send(d);});</script></body></html>
)=====";


// ==================================================================================
//   Implementasi Fungsi Publik
// ==================================================================================
void loadConfiguration() {
    Preferences preferences;
    Serial.println(F("Loading config from NVS..."));
    preferences.begin("device-config", true);
    bool prefsOpen = true;

    auto loadStr = [&](const char* key, char* buf, size_t size, const char* def) {
        String val = preferences.getString(key, "");
        if (val.length() == 0 || val == "null") {
            strncpy_P(buf, def, size - 1);
            buf[size - 1] = '\0';
        } else {
            strncpy(buf, val.c_str(), size - 1);
            buf[size - 1] = '\0';
        }
    };

    loadStr("ssid", active_ssid, sizeof(active_ssid), SSID);
    loadStr("pwd", active_pwd, sizeof(active_pwd), PWD);
    loadStr("token", active_api_token, sizeof(active_api_token), AUTH);
    loadStr("ta_token", active_ta_token, sizeof(active_ta_token), TA_AUTH);
    loadStr("th_url", active_th_url, sizeof(active_th_url), TH_URL);
    loadStr("nd_url_base", active_nd_url_base, sizeof(active_nd_url_base), ND_URL_BASE);
    loadStr("admin_pass", active_admin_pass, sizeof(active_admin_pass), DEFAULT_ADMIN_PASS);
    gprs_enabled = preferences.getBool("gprs_enabled", false);
    
    strncpy_P(active_device_status_post_url, DEVICE_STATUS_POST_URL, sizeof(active_device_status_post_url) - 1);
    active_device_status_post_url[sizeof(active_device_status_post_url) - 1] = '\0';
    
    char base_get_url[100];
    strncpy_P(base_get_url, DEVICE_STATUS_GET_URL_BASE, sizeof(base_get_url) - 1);
    base_get_url[sizeof(base_get_url) - 1] = '\0';
    snprintf(active_device_status_get_url, sizeof(active_device_status_get_url), "%s%d", base_get_url, GH_ID_CONFIG);
    // [AUTO-FIX] Detect and replace legacy "node-data" URL from NVS
    if (strstr(active_nd_url_base, "node-data") != NULL) {
        Serial.println(F("[CONFIG] Detected legacy 'node-data' URL in NVS. Migrating to new default..."));
        strncpy_P(active_nd_url_base, ND_URL_BASE, sizeof(active_nd_url_base) - 1);
        active_nd_url_base[sizeof(active_nd_url_base) - 1] = '\0';

        // Save the correction immediately (requires RW NVS handle)
        preferences.end();
        prefsOpen = false;
        preferences.begin("device-config", false);
        preferences.putString("nd_url_base", active_nd_url_base);
        preferences.end();
    }

    snprintf(active_nd_url, sizeof(active_nd_url), "%s%d", active_nd_url_base, GH_ID_CONFIG);
    
    if (prefsOpen) {
        preferences.end();
    }
    Serial.println(F("Config loaded."));
    Serial.printf("Node Data URL set to: %s\n", active_nd_url); // Tambahan log untuk verifikasi
    Serial.printf("GPRS enabled: %s\n", gprs_enabled ? "TRUE" : "FALSE");
}

bool verifyAdminPassword(const String& password)
{
    return password.length() > 0 && password == active_admin_pass;
}

bool verifyAdminPasswordWithRateLimit(const String& binding,
                                      const String& password,
                                      unsigned long* retryAfterMs)
{
    if (retryAfterMs)
        *retryAfterMs = 0;

    AdminAuthRateSlot* slot = findAuthRateSlot(binding);
    if (!slot)
        return false;

    slot->binding = normalizeBinding(binding);
    const unsigned long now = millis();
    if (slot->blocked)
    {
        const unsigned long elapsed = now - slot->blockedAtMs;
        if (elapsed <= ADMIN_AUTH_BLOCK_MS)
        {
            if (retryAfterMs)
                *retryAfterMs = ADMIN_AUTH_BLOCK_MS - elapsed;
            return false;
        }
        clearAuthRateSlot(*slot);
        slot->binding = normalizeBinding(binding);
    }

    if (slot->firstFailureMs != 0 && (now - slot->firstFailureMs) > ADMIN_AUTH_WINDOW_MS)
    {
        slot->failures = 0;
        slot->firstFailureMs = 0;
    }

    if (verifyAdminPassword(password))
    {
        clearAuthRateSlot(*slot);
        return true;
    }

    if (slot->firstFailureMs == 0)
        slot->firstFailureMs = now;
    if (slot->failures < 0xFF)
        ++slot->failures;
    if (slot->failures >= ADMIN_AUTH_MAX_FAILURES)
    {
        slot->blocked = true;
        slot->blockedAtMs = now;
        if (retryAfterMs)
            *retryAfterMs = ADMIN_AUTH_BLOCK_MS;
    }
    return false;
}

String issueAdminSessionToken(const String& binding)
{
    g_adminSessionToken = generateAdminSessionToken();
    g_adminSessionIssuedMs = millis();
    g_adminSessionBinding = normalizeBinding(binding);
    return g_adminSessionToken;
}

void invalidateAdminSessionToken()
{
    g_adminSessionToken = "";
    g_adminSessionIssuedMs = 0;
    g_adminSessionBinding = "";
}

bool isAdminSessionTokenValid(const String& token, const String& binding)
{
    if (token.length() == 0 || g_adminSessionToken.length() == 0)
        return false;
    if (token != g_adminSessionToken)
        return false;
    if ((millis() - g_adminSessionIssuedMs) > ADMIN_SESSION_TTL_MS)
        return false;
    const String normalizedBinding = normalizeBinding(binding);
    if (g_adminSessionBinding.length() > 0 && normalizedBinding.length() > 0 && g_adminSessionBinding != normalizedBinding)
        return false;
    return true;
}

String buildAdminRequestBinding(AsyncWebServerRequest* request)
{
    if (!request || !request->client())
        return "";
    return request->client()->remoteIP().toString();
}

String extractAdminTokenFromRequest(AsyncWebServerRequest* request)
{
    if (!request)
        return "";

    const String proofToken = extractAdminTokenFromProof(request);
    if (proofToken.length() > 0)
        return proofToken;

    if (request->hasHeader("X-Admin-Token"))
        return request->getHeader("X-Admin-Token")->value();

    if (request->hasHeader("Authorization"))
    {
        const String auth = request->getHeader("Authorization")->value();
        if (auth.startsWith("Bearer "))
            return auth.substring(7);
    }

    if (request->hasParam("admin_token"))
        return request->getParam("admin_token")->value();
    if (request->hasParam("admin_token", true))
        return request->getParam("admin_token", true)->value();

    return "";
}

bool isAdminRequestAuthorized(AsyncWebServerRequest* request, bool requireEncryptedProof)
{
    if (!request)
        return false;

    const String binding = buildAdminRequestBinding(request);
    JsonDocument proofDoc;
    if (parseAdminProofDocument(request, proofDoc))
    {
        const String proofToken = proofDoc["token"] | "";
        if (!isAdminSessionTokenValid(proofToken, binding))
            return false;
        if (requireEncryptedProof)
            return validateRequestProofScope(request, proofDoc);
        return true;
    }

    if (requireEncryptedProof)
        return false;

    return isAdminSessionTokenValid(extractAdminTokenFromRequest(request), binding);
}

void startConfigPortal(LCDDisplay& lcd) {
    AsyncWebServer server(80);
    DNSServer dnsServer;

    lcd.clear();
    lcd.message(0, 0, "CONFIG PORTAL MODE", false);
    
    WiFi.disconnect(true);
    delay(100);
    pinMode(MODEM_POWER_ON, OUTPUT);
    digitalWrite(MODEM_POWER_ON, LOW);

    String apSsid = "Gateway-Config-" + String(GH_ID_CONFIG);
    
    // Set mode AP_STA agar bisa scan networks
    WiFi.mode(WIFI_AP_STA);

    // FIX: Tambahkan kata sandi ke Access Point untuk mencegah akses tidak sah.
    const char* apPassword = "change-me-admin-password";
    WiFi.softAP(apSsid.c_str(), apPassword);
    
    lcd.message(0, 1, ("SSID: " + apSsid).c_str(), true);
    lcd.message(0, 2, ("PASS: " + String(apPassword)).c_str(), true);
    
    delay(500);
    IPAddress ip = WiFi.softAPIP();
    char ipStr[21];
    snprintf(ipStr, sizeof(ipStr), "IP: %s", ip.toString().c_str());
    lcd.message(0, 3, ipStr, false);

    dnsServer.start(53, "*", ip);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        String page = FPSTR(PORTAL_HTML);
        page.replace("%SSID%", String(active_ssid));
        page.replace("%TOKEN%", String(active_api_token));
        page.replace("%TA_TOKEN%", String(active_ta_token));
        page.replace("%TH_URL%", String(active_th_url));
        page.replace("%ND_URL_BASE%", String(active_nd_url_base));
        page.replace("%GPRS_CHECKED%", gprs_enabled ? "checked" : "");
        request->send(200, "text/html", page);
    });

    server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        int n = WiFi.scanNetworks();
        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();
        
        for (int i = 0; i < n; ++i) {
            JsonObject obj = array.add<JsonObject>();
            obj["ssid"] = WiFi.SSID(i);
            obj["rssi"] = WiFi.RSSI(i);
            obj["auth"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
        WiFi.scanDelete(); // Clean up RAM
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        Preferences preferences;
        String nSsid, nPass, nToken, nTaToken, nThUrl, nNdUrl, nAdminPass;
        const bool nGprsEnabled = request->hasParam("gprs_enabled", true);
        if(request->hasParam("ssid", true)) nSsid = request->getParam("ssid", true)->value();
        if(request->hasParam("pass", true)) nPass = request->getParam("pass", true)->value();
        if(request->hasParam("token", true)) nToken = request->getParam("token", true)->value();
        if(request->hasParam("ta_token", true)) nTaToken = request->getParam("ta_token", true)->value();
        if(request->hasParam("th_url", true)) nThUrl = request->getParam("th_url", true)->value();
        if(request->hasParam("nd_url_base", true)) nNdUrl = request->getParam("nd_url_base", true)->value();
        if(request->hasParam("admin_pass", true)) nAdminPass = request->getParam("admin_pass", true)->value();

        if (nSsid.length() > 0) {
            preferences.begin("device-config", false);
            preferences.putString("ssid", nSsid);
            if (nPass.length() > 0) preferences.putString("pwd", nPass);
            preferences.putString("token", nToken);
            if (nTaToken.length() > 0) preferences.putString("ta_token", nTaToken);
            preferences.putString("th_url", nThUrl);
            preferences.putString("nd_url_base", nNdUrl);
            if (nAdminPass.length() > 0) preferences.putString("admin_pass", nAdminPass);
            preferences.putBool("gprs_enabled", nGprsEnabled);
            preferences.end();

            if (nAdminPass.length() > 0) {
                strncpy(active_admin_pass, nAdminPass.c_str(), sizeof(active_admin_pass) - 1);
                active_admin_pass[sizeof(active_admin_pass) - 1] = '\0';
                invalidateAdminSessionToken();
            }
            gprs_enabled = nGprsEnabled;

            String msg = "Configuration Saved! Device will restart in 3 seconds.";
            request->send(200, "text/plain", msg);
            delay(3000);
            ESP.restart();
        } else {
            request->send(400, "text/plain", "Bad Request: SSID is required.");
        }
    });

    server.onNotFound([](AsyncWebServerRequest *request){
        request->redirect(String("http://") + WiFi.softAPIP().toString());
    });
    
    server.begin();
    unsigned long portalStartTime = millis();

    while (millis() - portalStartTime < PORTAL_TIMEOUT) {
        dnsServer.processNextRequest();
        yield();
    }
    
    ESP.restart();
}

void setupOtaHandlers(AsyncWebServer& server, LCDDisplay& lcd) {
    // Handler GET "/update" dihapus karena HTML-nya sekarang ada di main interface (WebSerial.cpp)

    // Handler POST "/doUpdate" TETAP DIBUTUHKAN untuk memproses file upload
    server.on("/doUpdate", HTTP_POST, 
        [&](AsyncWebServerRequest *request){
            auto* authCtx = static_cast<OtaUploadAuthContext*>(request->_tempObject);
            if (!authCtx || !authCtx->proofAuthorized) {
                clearOtaUploadAuthContext(request);
                request->send(403, "text/plain", "Forbidden");
                return;
            }
            const bool updateOk = !authCtx->uploadRejected && !Update.hasError();
            AsyncWebServerResponse *response = request->beginResponse(updateOk ? 200 : 500,
                                                                      "text/plain",
                                                                      updateOk ? "OK" : "FAIL");
            response->addHeader("Connection", "close");
            request->send(response);
            clearOtaUploadAuthContext(request);
            if (updateOk) {
                delay(1000);
                ESP.restart();
            }
        },
        [&](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
            OtaUploadAuthContext* authCtx = ensureOtaUploadAuthContext(request);
            if (!authCtx) {
                Update.abort();
                return;
            }
            if(!index){
                authCtx->proofAuthorized = isAdminRequestAuthorized(request, true);
                authCtx->uploadRejected = false;
                if (!authCtx->proofAuthorized) {
                    authCtx->uploadRejected = true;
                    Update.abort();
                    return;
                }
                authCtx->expectedFilename = request->getHeader("X-Upload-Filename")->value();
                authCtx->expectedSha256 = normalizeHexLower(request->getHeader("X-Upload-SHA256")->value());
                authCtx->expectedSize = static_cast<size_t>(request->getHeader("X-Upload-Size")->value().toInt());
                authCtx->bytesReceived = 0;
                authCtx->metadataValid = (authCtx->expectedFilename == filename) &&
                                         (authCtx->expectedSha256.length() == 64) &&
                                         (authCtx->expectedSize > 0);
                if (authCtx->metadataValid) {
                    authCtx->metadataValid = (sha256StartsCompat(&authCtx->shaCtx, 0) == 0);
                    authCtx->hashActive = authCtx->metadataValid;
                }
                if (!authCtx->metadataValid) {
                    authCtx->uploadRejected = true;
                    Update.abort();
                    return;
                }
                Serial.printf("Update Start: %s\n", filename.c_str());
                lcd.clear();
                lcd.message(0, 1, "OTA UPDATE START", true);
                if(!Update.begin(UPDATE_SIZE_UNKNOWN)){
                    authCtx->uploadRejected = true;
                    Update.printError(Serial);
                }
            }
            else if (!authCtx->proofAuthorized){
                authCtx->uploadRejected = true;
                Update.abort();
                return;
            }
            if(authCtx->hashActive && len > 0){
                if (sha256UpdateCompat(&authCtx->shaCtx, data, len) != 0) {
                    authCtx->uploadRejected = true;
                    authCtx->hashActive = false;
                    Update.abort();
                    return;
                }
                authCtx->bytesReceived += len;
            }
            if(!Update.hasError() && authCtx->metadataValid){
                if(Update.write(data, len) != len){
                    authCtx->uploadRejected = true;
                    Update.printError(Serial);
                }
            }
            if(final){
                if (authCtx->hashActive) {
                    unsigned char digest[32];
                    authCtx->hashActive = false;
                    if (sha256FinishCompat(&authCtx->shaCtx, digest) != 0) {
                        authCtx->uploadRejected = true;
                        Update.abort();
                        return;
                    }
                    const String actualSha = sha256ToHex(digest);
                    if (authCtx->bytesReceived != authCtx->expectedSize ||
                        actualSha != authCtx->expectedSha256) {
                        authCtx->uploadRejected = true;
                        Update.abort();
                    }
                }
                if(Update.end(true)){
                    Serial.printf("Update Success: %uB\n", index+len);
                } else {
                    authCtx->uploadRejected = true;
                    Update.printError(Serial);
                }
            }
        }
    );
}

void clearWiFiCredentials(LCDDisplay& lcd) {
    Preferences preferences;
    lcd.clear();
    lcd.message(0, 1, "Clearing WiFi...", true);
    preferences.begin("device-config", false);
    preferences.remove("ssid");
    preferences.remove("pwd");
    preferences.end();
    lcd.message(0, 2, "Restarting...", true);
    delay(2000);
}

void handleWifiReset(AsyncWebServer& server, LCDDisplay& lcd) {
    clearWiFiCredentials(lcd);
    ESP.restart();
}

void saveGPRSConfig(bool enabled) {
    Preferences preferences;
    preferences.begin("device-config", false);
    preferences.putBool("gprs_enabled", enabled);
    preferences.end();
    gprs_enabled = enabled;
    Serial.printf("[CONFIG] GPRS setting saved: %s\n", enabled ? "TRUE" : "FALSE");
}
