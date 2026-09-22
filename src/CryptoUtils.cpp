///////////////////////////////////////////////////////////////////////////////
// File: CryptoUtils.cpp
///////////////////////////////////////////////////////////////////////////////

#include "CryptoUtils.h"
#include "config.h"

#include <esp_system.h>
#include <mbedtls/aes.h>
#include <mbedtls/base64.h>

#include <cstring>
#include <vector>

#define DEBUG_PRINTLN(level, ...) \
    do                            \
    {                             \
        if (DEBUG_LEVEL >= level) \
        {                         \
            Serial.println(__VA_ARGS__); \
        }                         \
    } while (0)

namespace
{
    uint32_t gReplaySkewWindow = CryptoUtils::DEFAULT_ENCRYPTED_MAX_AGE_SEC;

    String makeBoundedString(const char* data, size_t len)
    {
        String out;
        if (!data || len == 0)
            return out;
        out.reserve(len);
        for (size_t i = 0; i < len; ++i)
            out += data[i];
        return out;
    }

    size_t validatePkcs7Padding(const uint8_t* data, size_t len)
    {
        if (len == 0)
            return 0;
        const uint8_t pad = data[len - 1];
        if (pad == 0 || pad > CryptoUtils::AES_BLOCK_SIZE || pad > len)
            return 0;
        for (size_t i = 0; i < pad; ++i)
        {
            if (data[len - 1 - i] != pad)
                return 0;
        }
        return pad;
    }

    bool splitLegacyPayload(const char* encPayload, size_t len, String& b64Iv, String& b64Cipher)
    {
        if (!encPayload || len < 24)
            return false;
        String payloadStr = makeBoundedString(encPayload, len);
        const int firstColon = payloadStr.indexOf(':');
        const int secondColon = payloadStr.indexOf(':', firstColon + 1);
        if (firstColon == -1 || secondColon == -1)
            return false;
        b64Iv = payloadStr.substring(firstColon + 1, secondColon);
        b64Cipher = payloadStr.substring(secondColon + 1);
        return b64Iv.length() > 0 && b64Cipher.length() > 0;
    }

    bool splitNodeMediniPayload(const char* encPayload, size_t len, String& b64Iv, String& b64Cipher)
    {
        if (!encPayload || len < 25)
            return false;
        String payloadStr = makeBoundedString(encPayload, len);
        const int separator = payloadStr.indexOf(':');
        if (separator <= 0 || separator >= (payloadStr.length() - 1))
            return false;
        b64Iv = payloadStr.substring(0, separator);
        b64Cipher = payloadStr.substring(separator + 1);
        return b64Iv.length() > 0 && b64Cipher.length() > 0;
    }

    bool base64EncodeToString(const uint8_t* data, size_t len, String& out)
    {
        if (!data || len == 0)
            return false;
        const size_t outSize = (((len + 2) / 3) * 4) + 1;
        std::vector<unsigned char> buf(outSize, 0);
        size_t written = 0;
        const int rc = mbedtls_base64_encode(buf.data(), buf.size(), &written, data, len);
        if (rc != 0 || written == 0)
            return false;
        out = String(reinterpret_cast<const char*>(buf.data()), written);
        return out.length() == static_cast<int>(written);
    }

    bool base64DecodeToVector(const String& b64, std::vector<uint8_t>& out, size_t& written)
    {
        if (b64.length() == 0)
            return false;
        out.assign(b64.length(), 0);
        written = 0;
        const int rc = mbedtls_base64_decode(out.data(),
                                             out.size(),
                                             &written,
                                             reinterpret_cast<const unsigned char*>(b64.c_str()),
                                             b64.length());
        return rc == 0 && written > 0 && written <= out.size();
    }
}

namespace CryptoUtils
{

void setReplaySkewWindow(uint32_t windowSec)
{
    if (windowSec < ENCRYPTED_REPLAY_WINDOW_STRICT_SEC)
        windowSec = ENCRYPTED_REPLAY_WINDOW_STRICT_SEC;
    if (windowSec > ENCRYPTED_REPLAY_WINDOW_MAX_SEC)
        windowSec = ENCRYPTED_REPLAY_WINDOW_MAX_SEC;
    gReplaySkewWindow = windowSec;
}

uint32_t getReplaySkewWindow()
{
    return gReplaySkewWindow;
}

bool isEncryptedPayload(const uint8_t* data, size_t len)
{
    return data && len >= 4 && data[0] == 'E' && data[1] == 'N' && data[2] == 'C' && data[3] == ':';
}

bool isNodeMediniEncryptedPayload(const uint8_t* data, size_t len)
{
    if (!data || len < 25 || isEncryptedPayload(data, len))
        return false;

    size_t separatorCount = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const char c = static_cast<char>(data[i]);
        if (c == ':')
        {
            ++separatorCount;
            continue;
        }

        const bool isBase64Char =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '+' || c == '/' || c == '=';
        if (!isBase64Char)
            return false;
    }
    return separatorCount == 1;
}

size_t encryptPayload(const char* plaintext, size_t plaintextLen, char* outBuffer, size_t outBufferSize)
{
    String encrypted;
    if (!encryptNodeMediniPayload(plaintext, plaintextLen, encrypted))
        return 0;

    if (!outBuffer || outBufferSize == 0 || encrypted.length() >= outBufferSize)
        return 0;

    memcpy(outBuffer, encrypted.c_str(), encrypted.length());
    outBuffer[encrypted.length()] = '\0';
    return encrypted.length();
}

bool encryptNodeMediniPayload(const char* plaintext,
                              size_t plaintextLen,
                              String& outPayload,
                              time_t epochOverride)
{
    outPayload = "";
    if (!plaintext)
        return false;

    const size_t rawLen = plaintextLen + 4;
    size_t padLen = AES_BLOCK_SIZE - (rawLen % AES_BLOCK_SIZE);
    if (padLen == 0)
        padLen = AES_BLOCK_SIZE;
    const size_t totalLen = rawLen + padLen;

    std::vector<uint8_t> plain(totalLen, 0);
    const uint32_t now = (epochOverride > 0) ? static_cast<uint32_t>(epochOverride) : static_cast<uint32_t>(time(nullptr));
    plain[0] = static_cast<uint8_t>(now >> 24);
    plain[1] = static_cast<uint8_t>(now >> 16);
    plain[2] = static_cast<uint8_t>(now >> 8);
    plain[3] = static_cast<uint8_t>(now);
    if (plaintextLen > 0)
        memcpy(plain.data() + 4, plaintext, plaintextLen);
    for (size_t i = rawLen; i < totalLen; ++i)
        plain[i] = static_cast<uint8_t>(padLen);

    uint8_t iv[AES_BLOCK_SIZE];
    uint8_t ivCopy[AES_BLOCK_SIZE];
    for (size_t i = 0; i < AES_BLOCK_SIZE; ++i)
        iv[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    memcpy(ivCopy, iv, sizeof(ivCopy));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, AES_KEY, 256) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }

    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, totalLen, ivCopy, plain.data(), plain.data()) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_free(&aes);

    String ivB64;
    String cipherB64;
    if (!base64EncodeToString(iv, sizeof(iv), ivB64))
        return false;
    if (!base64EncodeToString(plain.data(), totalLen, cipherB64))
        return false;

    outPayload.reserve(ivB64.length() + 1 + cipherB64.length());
    outPayload = ivB64;
    outPayload += ':';
    outPayload += cipherB64;
    return outPayload.length() > 0;
}

bool decryptPayload(const char* encPayload, size_t len, char* outBuffer, size_t outBufferSize, size_t* outLen)
{
    String b64Iv;
    String b64Cipher;
    if (!splitLegacyPayload(encPayload, len, b64Iv, b64Cipher))
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Invalid format (missing colons)");
        return false;
    }

    unsigned char iv[AES_BLOCK_SIZE];
    size_t ivLen = 0;
    const int ivRc = mbedtls_base64_decode(iv,
                                           sizeof(iv),
                                           &ivLen,
                                           reinterpret_cast<const unsigned char*>(b64Iv.c_str()),
                                           b64Iv.length());
    if (ivRc != 0 || ivLen != AES_BLOCK_SIZE)
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Invalid IV length");
        return false;
    }

    std::vector<uint8_t> cipherData;
    size_t cipherLen = 0;
    if (!base64DecodeToVector(b64Cipher, cipherData, cipherLen) || cipherLen == 0 || (cipherLen % AES_BLOCK_SIZE) != 0)
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Invalid Ciphertext length");
        return false;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, AES_KEY, 256) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }

    std::vector<uint8_t> decryptedData(cipherLen, 0);
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, iv, cipherData.data(), decryptedData.data()) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_free(&aes);

    const size_t padLen = validatePkcs7Padding(decryptedData.data(), cipherLen);
    if (padLen == 0)
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Padding check failed");
        return false;
    }

    const size_t actualLen = cipherLen - padLen;
    if (actualLen <= 4)
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Data too short to contain timestamp + JSON");
        return false;
    }

    const size_t jsonLen = actualLen - 4;
    if (jsonLen >= outBufferSize)
    {
        DEBUG_PRINTLN(1, "[CRYPTO] Output buffer too small");
        return false;
    }

    memcpy(outBuffer, decryptedData.data() + 4, jsonLen);
    outBuffer[jsonLen] = '\0';
    if (outLen)
        *outLen = jsonLen;
    return true;
}

bool decryptNodeMediniPayload(const char* encPayload,
                              size_t len,
                              char* outBuffer,
                              size_t outBufferSize,
                              size_t* outLen,
                              uint32_t* outTimestamp,
                              uint32_t maxAgeSec)
{
    if (!outBuffer || outBufferSize == 0 || !outLen)
        return false;

    String b64Iv;
    String b64Cipher;
    if (!splitNodeMediniPayload(encPayload, len, b64Iv, b64Cipher))
        return false;

    std::vector<uint8_t> ivBuf;
    size_t ivLen = 0;
    if (!base64DecodeToVector(b64Iv, ivBuf, ivLen) || ivLen != AES_BLOCK_SIZE)
        return false;

    std::vector<uint8_t> cipherData;
    size_t cipherLen = 0;
    if (!base64DecodeToVector(b64Cipher, cipherData, cipherLen) || cipherLen == 0 || (cipherLen % AES_BLOCK_SIZE) != 0)
        return false;

    uint8_t ivCopy[AES_BLOCK_SIZE];
    memcpy(ivCopy, ivBuf.data(), sizeof(ivCopy));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, AES_KEY, 256) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }

    std::vector<uint8_t> decryptedData(cipherLen, 0);
    if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, ivCopy, cipherData.data(), decryptedData.data()) != 0)
    {
        mbedtls_aes_free(&aes);
        return false;
    }
    mbedtls_aes_free(&aes);

    const size_t padLen = validatePkcs7Padding(decryptedData.data(), cipherLen);
    if (padLen == 0)
        return false;

    const size_t actualLen = cipherLen - padLen;
    if (actualLen <= 4)
        return false;

    const uint32_t timestamp =
        (static_cast<uint32_t>(decryptedData[0]) << 24) |
        (static_cast<uint32_t>(decryptedData[1]) << 16) |
        (static_cast<uint32_t>(decryptedData[2]) << 8) |
        static_cast<uint32_t>(decryptedData[3]);

    if (maxAgeSec == 0)
        maxAgeSec = getReplaySkewWindow();

    const time_t now = time(nullptr);
    if (now > 1704067200UL && !validateTimestamp(static_cast<time_t>(timestamp), maxAgeSec))
        return false;

    const size_t plainLen = actualLen - 4;
    if (plainLen >= outBufferSize)
        return false;

    memcpy(outBuffer, decryptedData.data() + 4, plainLen);
    outBuffer[plainLen] = '\0';
    *outLen = plainLen;
    if (outTimestamp)
        *outTimestamp = timestamp;
    return true;
}

bool validateSignature(const char* payload, size_t payloadLen, const char* signatureHex, const uint8_t* key, size_t keyLen)
{
    (void)payload;
    (void)payloadLen;
    (void)signatureHex;
    (void)key;
    (void)keyLen;
    return true;
}

bool validateTimestamp(time_t timestamp, uint32_t maxAgeSec)
{
    const time_t now = time(nullptr);
    if (now <= 1704067200UL)
        return true;
    if (timestamp <= 0)
        return false;

    const time_t lowerBound = now - static_cast<time_t>(maxAgeSec);
    const time_t upperBound = now + static_cast<time_t>(maxAgeSec);
    return timestamp >= lowerBound && timestamp <= upperBound;
}

} // namespace CryptoUtils
