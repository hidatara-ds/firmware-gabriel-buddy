// ============================================================
// audio_utils.h — Pure audio utility functions
// ============================================================
#ifndef AUDIO_UTILS_H
#define AUDIO_UTILS_H

#include <mbedtls/base64.h>

// ── PCM Conversion ──────────────────────────────────────────
inline int16_t convertI2SSampleToPCM16(int32_t sample32) {
    int32_t s = sample32 >> I2S_SAMPLE_SHIFT;
    if (s > 32767)  s =  32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

// ── WAV Header Writer ───────────────────────────────────────
void writeWavHeader(uint8_t* header, uint32_t pcmDataSize, uint32_t sampleRate) {
    const uint16_t numChannels = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const uint32_t chunkSize = 36 + pcmDataSize;

    memcpy(header + 0, "RIFF", 4);
    header[4] = chunkSize & 0xFF;
    header[5] = (chunkSize >> 8) & 0xFF;
    header[6] = (chunkSize >> 16) & 0xFF;
    header[7] = (chunkSize >> 24) & 0xFF;
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size (PCM)
    header[20] = 1; header[21] = 0; // AudioFormat (PCM)
    header[22] = numChannels & 0xFF;
    header[23] = (numChannels >> 8) & 0xFF;
    header[24] = sampleRate & 0xFF;
    header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF;
    header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = blockAlign & 0xFF;
    header[33] = (blockAlign >> 8) & 0xFF;
    header[34] = bitsPerSample & 0xFF;
    header[35] = (bitsPerSample >> 8) & 0xFF;
    memcpy(header + 36, "data", 4);
    header[40] = pcmDataSize & 0xFF;
    header[41] = (pcmDataSize >> 8) & 0xFF;
    header[42] = (pcmDataSize >> 16) & 0xFF;
    header[43] = (pcmDataSize >> 24) & 0xFF;
}

// ── Base64 Encoding ─────────────────────────────────────────
bool base64Encode(const uint8_t* data, size_t dataLen, String& out) {
    size_t encodedLen = 0;
    int rc = mbedtls_base64_encode(nullptr, 0, &encodedLen, data, dataLen);
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || encodedLen == 0) {
        return false;
    }

    unsigned char* encoded = (unsigned char*)malloc(encodedLen + 1);
    if (!encoded) return false;

    rc = mbedtls_base64_encode(encoded, encodedLen, &encodedLen, data, dataLen);
    if (rc != 0) {
        free(encoded);
        return false;
    }

    encoded[encodedLen] = '\0';
    out = String((char*)encoded);
    free(encoded);
    return true;
}

// ── JSON String Field Extractor ─────────────────────────────
String extractJsonStringField(const String& json, const char* key) {
    String token = "\"";
    token += key;
    token += "\":\"";
    int start = json.indexOf(token);
    if (start < 0) return "";
    start += token.length();

    String out;
    out.reserve(128);
    bool escape = false;
    for (int i = start; i < json.length(); i++) {
        char c = json[i];
        if (escape) {
            switch (c) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                default: out += c; break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            break;
        }
        out += c;
    }
    return out;
}

// ── Mono-to-Stereo with Volume/Gain ─────────────────────────
size_t monoToStereoWithGain(const uint8_t* monoPcmBytes, size_t monoByteLen, uint8_t* stereoOut, size_t stereoOutCapacity) {
    if (monoPcmBytes == nullptr || stereoOut == nullptr || monoByteLen < 2 || stereoOutCapacity < 4) return 0;
    size_t monoSampleCount = monoByteLen / sizeof(int16_t);
    size_t maxStereoSamples = stereoOutCapacity / (sizeof(int16_t) * 2);
    if (monoSampleCount > maxStereoSamples) monoSampleCount = maxStereoSamples;

    const int16_t* mono = (const int16_t*)monoPcmBytes;
    int16_t* stereo = (int16_t*)stereoOut;
    for (size_t i = 0; i < monoSampleCount; i++) {
        int32_t scaled = ((int32_t)mono[i] * (int32_t)SPEAKER_VOLUME_PERCENT) / 100;
        scaled = (scaled * (int32_t)SPEAKER_GAIN_PERCENT) / 100;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        int16_t sample = (int16_t)scaled;
        stereo[i * 2] = sample;      // Left
        stereo[i * 2 + 1] = sample;  // Right
    }
    return monoSampleCount * sizeof(int16_t) * 2;
}

// ── API Host Helpers ────────────────────────────────────────
String getApiHost() {
    String base = String(API_BASE_URL);
    if (base.startsWith("https://")) base = base.substring(8);
    if (base.startsWith("http://")) base = base.substring(7);
    int slash = base.indexOf('/');
    if (slash > 0) base = base.substring(0, slash);
    return base;
}

bool isApiTls() {
    String base = String(API_BASE_URL);
    return base.startsWith("https://");
}

#endif
