#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <Arduino.h>
#include <time.h>

namespace CryptoUtils {

// KUNCI: "abcdefghijklmnopqrstuvwxyz123456"
// Kunci ini HARUS sama dengan yang ada di WebSerial.cpp
constexpr uint8_t AES_KEY[] = {
    '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '1', '2', '3', '4', '5', '6',
    '7', '8', '9', '0', '1', '2', '3', '4',
    '5', '6', '7', '8', '9', '0', '1', '2'
};

constexpr size_t AES_KEY_SIZE = 32;
constexpr size_t AES_BLOCK_SIZE = 16;
constexpr size_t MAX_DECRYPTED_SIZE = 1536;
// Ikuti window replay node medini: strict 30s, soft 5m, max 15m.
// Default gateway memakai soft window agar toleran saat jam client/server sedikit meleset.
constexpr uint32_t ENCRYPTED_REPLAY_WINDOW_STRICT_SEC = 30;
constexpr uint32_t ENCRYPTED_REPLAY_WINDOW_SOFT_SEC = 300;
constexpr uint32_t ENCRYPTED_REPLAY_WINDOW_MAX_SEC = 900;
constexpr uint32_t DEFAULT_ENCRYPTED_MAX_AGE_SEC = ENCRYPTED_REPLAY_WINDOW_SOFT_SEC;

bool isEncryptedPayload(const uint8_t* data, size_t len);
size_t encryptPayload(const char* plaintext, size_t plaintextLen, char* outBuffer, size_t outBufferSize);
bool isNodeMediniEncryptedPayload(const uint8_t* data, size_t len);
bool encryptNodeMediniPayload(const char* plaintext,
                              size_t plaintextLen,
                              String& outPayload,
                              time_t epochOverride = 0);
void setReplaySkewWindow(uint32_t windowSec);
uint32_t getReplaySkewWindow();
bool decryptNodeMediniPayload(const char* encPayload,
                              size_t len,
                              char* outBuffer,
                              size_t outBufferSize,
                              size_t* outLen,
                              uint32_t* outTimestamp = nullptr,
                              uint32_t maxAgeSec = 0);

// Fungsi pelengkap (agar tidak error link)
bool decryptPayload(const char* encPayload, size_t len, char* outBuffer, size_t outBufferSize, size_t* outLen);
bool validateSignature(const char* payload, size_t payloadLen, const char* signatureHex, const uint8_t* key, size_t keyLen);
bool validateTimestamp(time_t timestamp, uint32_t maxAgeSec = 300);

}
#endif
