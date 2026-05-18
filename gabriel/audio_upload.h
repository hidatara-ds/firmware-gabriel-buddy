// ============================================================
// audio_upload.h — HTTP audio upload (fallback) + postAudio dispatcher
// ============================================================
#ifndef AUDIO_UPLOAD_H
#define AUDIO_UPLOAD_H

bool postAudio() {
    currentCategory[0] = '\0';
    currentMessage[0] = '\0';

    if (USE_SOCKET_IO && postAudioViaWebSocket()) {
        return true;
    }

    if (!ensureWiFi()) {
        strncpy(currentMessage, "WiFi terputus", MAX_MSG_LENGTH);
        return false;
    }

    if (recordPeakLevel < MIN_UPLOAD_PEAK) {
        if (DEBUG_SERIAL) {
            Serial.printf("[AUDIO] Skip upload: peak too low (%d < %d)\n", recordPeakLevel, MIN_UPLOAD_PEAK);
        }
        strncpy(currentMessage, "Suara terlalu pelan", MAX_MSG_LENGTH - 1);
        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
        strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
        currentCategory[sizeof(currentCategory) - 1] = '\0';
        return false;
    }

    HTTPClient http;
    String url = String(API_BASE_URL) + API_ENDPOINT;

    if (DEBUG_SERIAL) Serial.printf("[API] Preparing %d bytes PCM for: %s\n", audioDataSize, url.c_str());

    const size_t wavSize = audioDataSize + 44;
    uint8_t* wavBuffer = nullptr;
    if (psramFound()) {
        wavBuffer = (uint8_t*)ps_malloc(wavSize);
    }
    if (!wavBuffer) {
        wavBuffer = (uint8_t*)malloc(wavSize);
    }
    if (!wavBuffer) {
        if (DEBUG_SERIAL) Serial.printf("[API] WAV alloc failed: need %u bytes, free heap=%u\n",
                                        (unsigned)wavSize, ESP.getFreeHeap());
        strncpy(currentMessage, "WAV alloc failed", MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        return false;
    }

    writeWavHeader(wavBuffer, (uint32_t)audioDataSize, I2S_SAMPLE_RATE);
    memcpy(wavBuffer + 44, audioBuffer, audioDataSize);

    String audioBase64;
    if (!base64Encode(wavBuffer, wavSize, audioBase64)) {
        free(wavBuffer);
        strncpy(currentMessage, "Base64 encode failed", MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        return false;
    }
    free(wavBuffer);

    String requestBody;
    requestBody.reserve(audioBase64.length() + 96);
    requestBody = "{\"audio\":\"";
    requestBody += audioBase64;
    requestBody += "\",\"include_audio\":true,\"audio_delivery\":\"url\"}";

    if (DEBUG_SERIAL) {
        float recordSeconds = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
        Serial.printf("[AUDIO] PCM bytes: %u | WAV bytes: %u | b64 chars: %u | rec: %.2fs\n",
                      (unsigned int)audioDataSize,
                      (unsigned int)wavSize,
                      (unsigned int)audioBase64.length(),
                      recordSeconds);
    }

    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Client", "esp32");
    if (DEBUG_SERIAL) {
        Serial.printf("[API] Upload bytes(json): %d | Free heap: %u\n", requestBody.length(), ESP.getFreeHeap());
    }

    int httpCode = http.POST((uint8_t*)requestBody.c_str(), requestBody.length());
    if (httpCode < 0) {
        if (DEBUG_SERIAL) {
            Serial.printf("[API] Transport error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
            Serial.println("[API] Retrying request once...");
        }
        http.end();
        delay(250);
        http.begin(url);
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("X-Client", "esp32");
        httpCode = http.POST((uint8_t*)requestBody.c_str(), requestBody.length());
    }

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        JsonDocument filter;
        filter["answer"] = true;
        filter["message"] = true;
        filter["question"] = true;
        filter["stage"] = true;
        filter["error"] = true;
        filter["audio_url"] = true;
        filter["audio_id"] = true;
        filter["audio_mime"] = true;
        filter["audio_encoding"] = true;

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
        if (error) {
            String fallbackAnswer = extractJsonStringField(payload, "answer");
            if (fallbackAnswer.length() > 0) {
                strncpy(currentMessage, fallbackAnswer.c_str(), MAX_MSG_LENGTH - 1);
                currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                currentCategory[sizeof(currentCategory) - 1] = '\0';
                strncpy(currentEmoji, "AI", sizeof(currentEmoji) - 1);
                currentEmoji[sizeof(currentEmoji) - 1] = '\0';
                if (DEBUG_SERIAL) {
                    Serial.printf("[API] JSON parse fallback OK | payload_len=%u\n", (unsigned int)payload.length());
                    Serial.printf("[MSG] %s | %s: %s\n", currentEmoji, currentCategory, currentMessage);
                }
                http.end();
                return true;
            }

            if (DEBUG_SERIAL) {
                String preview = payload.substring(0, 220);
                Serial.printf("[API] JSON parse error: %s | payload_len=%u | preview=%s\n",
                              error.c_str(), (unsigned int)payload.length(), preview.c_str());
            }
            strncpy(currentMessage, "JSON parse error", MAX_MSG_LENGTH);
            strncpy(currentCategory, "error", sizeof(currentCategory));
            http.end();
            return false;
        }

        strncpy(currentMessage, doc["answer"] | doc["message"] | "No message", MAX_MSG_LENGTH - 1);
        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
        strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
        strncpy(currentEmoji, "AI", sizeof(currentEmoji) - 1);

        const char* audioUrl = doc["audio_url"] | "";
        if (strlen(audioUrl) > 0) {
            pendingAudioUrl = String(API_BASE_URL) + audioUrl;
        } else {
            pendingAudioUrl = "";
        }

        if (DEBUG_SERIAL) {
            const char* question = doc["question"] | "";
            Serial.printf("[API] OK 200 | question_len=%u | answer_len=%u\n",
                          (unsigned int)strlen(question),
                          (unsigned int)strlen(currentMessage));
            if (strlen(question) > 0) {
                Serial.printf("[API] STT: %s\n", question);
            }
            Serial.printf("[MSG] %s | %s: %s\n", currentEmoji, currentCategory, currentMessage);
        }

        http.end();
        return true;
    } else {
        String errPayload = http.getString();
        if (DEBUG_SERIAL) {
            if (httpCode < 0) {
                Serial.printf("[API] Transport error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
            } else {
                Serial.printf("[API] HTTP Error: %d | %s\n", httpCode, errPayload.c_str());
            }
        }
        char errMsg[MAX_MSG_LENGTH];
        if (httpCode < 0) {
            snprintf(errMsg, MAX_MSG_LENGTH, "NET Error: %d", httpCode);
        } else {
            snprintf(errMsg, MAX_MSG_LENGTH, "HTTP Error: %d", httpCode);
        }
        strncpy(currentMessage, errMsg, MAX_MSG_LENGTH);
        strncpy(currentCategory, "error", sizeof(currentCategory));
        http.end();
        return false;
    }
}

#endif
