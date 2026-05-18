// ============================================================
// ☁️ GABRIEL — Cloud Messenger for ESP32-S3
// Fetch AI-generated messages and display on OLED
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>   // Native WS — supports binary frames
#include <U8g2lib.h>
#include <Wire.h>
#include <esp_system.h>
#include <driver/i2s_std.h>
#include <mbedtls/base64.h>
#include "config.h"

// ── App States ──────────────────────────────────────────────
enum AppState {
    STATE_STANDBY,
    STATE_RECORDING,
    STATE_PROCESSING,
    STATE_DISPLAY
};

// ── I2S Mic Config ──────────────────────────────────────────
#define I2S_PORT I2S_NUM_0
#ifndef RECORD_TIME
#define RECORD_TIME 4 // max 4 seconds
#endif
#ifndef MIN_UPLOAD_PEAK
#define MIN_UPLOAD_PEAK 300
#endif
#ifndef MIC_DIAG_ON_BOOT
#define MIC_DIAG_ON_BOOT true
#endif
#ifndef MIC_DIAG_DURATION_MS
#define MIC_DIAG_DURATION_MS 3000
#endif
#ifndef VAD_START_HITS
#define VAD_START_HITS 4
#endif
#ifndef VAD_STOP_THRESHOLD
#define VAD_STOP_THRESHOLD 60
#endif
#ifndef VAD_SILENCE_MS
#define VAD_SILENCE_MS 700
#endif
#ifndef MIN_RECORD_MS
#define MIN_RECORD_MS 700
#endif
#ifndef I2S_SAMPLE_SHIFT
#define I2S_SAMPLE_SHIFT 14
#endif
#ifndef INMP441_USE_LEFT
#define INMP441_USE_LEFT true
#endif
#ifndef SPEAKER_VOLUME_PERCENT
#define SPEAKER_VOLUME_PERCENT 95
#endif
#ifndef SPEAKER_GAIN_PERCENT
#define SPEAKER_GAIN_PERCENT 130
#endif
#ifndef AUDIO_STREAM_IDLE_TIMEOUT_MS
#define AUDIO_STREAM_IDLE_TIMEOUT_MS 1200
#endif

// ── Global State ────────────────────────────────────────────
const size_t RECORD_SIZE = I2S_SAMPLE_RATE * 2 * RECORD_TIME;
uint8_t* audioBuffer = nullptr;
size_t audioDataSize = 0;
i2s_chan_handle_t rxChan = nullptr;
i2s_chan_handle_t txChan = nullptr;
String pendingAudioUrl = "";

// ── Ticker + Speaking Animation ─────────────────────────────
String tickerText    = "";    // answer text scrolling below face
int    tickerOffset  = 0;     // pixels scrolled (increases over time)
unsigned long lastTickerMs = 0;
unsigned long lastRequestMs = 0;
int speakingFrame = 0;        // 0-3 for mouth open/close animation

// ── Streaming audio queue ───────────────────────────────────
#define AUDIO_QUEUE_SIZE 8
String audioUrlQueue[AUDIO_QUEUE_SIZE];
int    audioQueueHead = 0;
int    audioQueueTail = 0;
bool   wsStreamDone   = false;   // server sent "done" packet

// ── WebSocket (raw binary, Socket.IO-like protocol) ─────────
WebSocketsClient wsClient;
bool wsConnected   = false;
bool wsHandshook   = false;   // received 'connected' event from server
String wsSessionId = "";
uint8_t wsSidBuf[24] = {0};    // Engine.IO sid buffer

// ── App State ───────────────────────────────────────────────
char currentMessage[MAX_MSG_LENGTH] = "";
char currentCategory[16] = "";
char currentEmoji[8] = "";
int fetchCount = 0;
int errorCount = 0;
bool wifiConnected = false;
unsigned long recordStartMs = 0;
unsigned long lastRecordProgressLogMs = 0;
int recordPeakLevel = 0;
int vadHitCount = 0;
unsigned long silenceStartMs = 0;

AppState currentState = STATE_STANDBY;
unsigned long stateStartTime = 0;

// ═══════════════════════════════════════════════════════════
// MODULE INCLUDES (order matters for function visibility)
// ═══════════════════════════════════════════════════════════
#include "audio_utils.h"        // Pure utilities: PCM convert, WAV header, base64, etc.
#include "i2s_driver.h"         // I2S mic/speaker init and helpers
#include "display_face.h"       // OLED display, face animation, boot screen
#include "wifi_manager.h"       // WiFi connect/reconnect
#include "audio_playback.h"     // playAudioFromUrl (WAV download + I2S playback)
#include "websocket_handler.h"  // Socket.IO protocol, streaming, postAudioViaWebSocket
#include "audio_upload.h"       // postAudio (HTTP fallback + dispatcher)

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(115200);
    delay(500);
    
    Serial.println();
    Serial.println("============================================");
    Serial.println("  GABRIEL — Cloud Messenger for ESP32-S3");
    Serial.println("  Version 1.0.0");
    Serial.println("============================================");
    
    // Init OLED
    display.begin();
    display.setContrast(200);
    display.clearBuffer();
    display.sendBuffer();
    
    // Boot animation
    showBootAnimation();
    
    // Init I2S Mic & Speaker
    initI2SMic();
    initI2SSpeaker();

    if (DEBUG_SERIAL && MIC_DIAG_ON_BOOT) {
        runMicDiagnostic(MIC_DIAG_DURATION_MS);
    }
    
    // Prefer PSRAM for the large audio capture buffer so that regular heap
    // stays free for HTTP/WS stacks and the WAV envelope allocation later.
    if (psramFound()) {
        audioBuffer = (uint8_t*)ps_malloc(RECORD_SIZE);
        if (audioBuffer) Serial.printf("[AUDIO] audioBuffer in PSRAM: %u bytes\n", (unsigned)RECORD_SIZE);
    }
    if (!audioBuffer) {
        audioBuffer = (uint8_t*)malloc(RECORD_SIZE);
        if (audioBuffer) Serial.printf("[AUDIO] audioBuffer in heap: %u bytes\n", (unsigned)RECORD_SIZE);
    }
    if (!audioBuffer) {
        Serial.println("[ERROR] Failed to allocate audio buffer — out of memory!");
    }
    
    // Connect WiFi
    if (!connectWiFi()) {
        showError("WiFi Failed!", "Check SSID & password in config.h");
    }
    
    Serial.println("[SETUP] ✓ Gabriel ready!");
    Serial.printf("[SETUP] API: %s%s\n", API_BASE_URL, API_ENDPOINT);
    Serial.printf("[SETUP] Audio cfg: %d Hz, shift=%d, RECORD_TIME=%ds, REC_BUF=%u bytes, MIN_UPLOAD_PEAK=%d\n",
                  I2S_SAMPLE_RATE, I2S_SAMPLE_SHIFT, RECORD_TIME, (unsigned int)RECORD_SIZE, MIN_UPLOAD_PEAK);
    Serial.println("[SETUP] Entering VAD Standby Mode...");
    
    randomSeed((uint32_t)esp_random());
    updateFaceAnimation(FACE_IDLE, true);
}

// ═══════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
    unsigned long now = millis();
    
    switch (currentState) {
        case STATE_STANDBY: {
            int32_t sampleBuffer[256];
            size_t bytesRead = 0;
            
            // Read a small chunk of audio to detect voice
            if (rxChan != nullptr) {
                i2s_channel_read(rxChan, sampleBuffer, sizeof(sampleBuffer), &bytesRead, pdMS_TO_TICKS(20));
            }
            updateFaceAnimation(FACE_IDLE);
            
            int samples = bytesRead / sizeof(int32_t);
            if (samples > 0) {
                long sum = 0;
                for (int i = 0; i < samples; i++) {
                    int16_t pcm = convertI2SSampleToPCM16(sampleBuffer[i]);
                    sum += abs(pcm);
                }
                int avgEnergy = sum / samples;
                
                if (avgEnergy > VAD_THRESHOLD) {
                    if (vadHitCount < VAD_START_HITS) vadHitCount++;
                } else if (vadHitCount > 0) {
                    vadHitCount--;
                }

                if (avgEnergy > VAD_THRESHOLD && DEBUG_SERIAL) {
                    Serial.printf("[VAD] energy=%d thr=%d hits=%d/%d\n",
                                  avgEnergy, VAD_THRESHOLD, vadHitCount, VAD_START_HITS);
                }

                if (vadHitCount >= VAD_START_HITS) {
                    if (DEBUG_SERIAL) Serial.println("[VAD] Trigger confirmed -> RECORDING");
                    
                    currentState = STATE_RECORDING;
                    audioDataSize = 0;
                    recordStartMs = millis();
                    lastRecordProgressLogMs = recordStartMs;
                    recordPeakLevel = avgEnergy;
                    silenceStartMs = 0;
                    vadHitCount = 0;
                    flushI2SInput(); // flush to start clean recording
                    
                    updateFaceAnimation(FACE_LISTENING, true);
                }
            }
            break;
        }
        
        case STATE_RECORDING: {
            bool stopBySilence = false;
            bool stopByBufferFull = false;
            if (audioBuffer && audioDataSize < RECORD_SIZE) {
                int32_t i2sChunk[256];
                size_t bytesRead = 0;
                if (rxChan != nullptr) {
                    i2s_channel_read(rxChan, i2sChunk, sizeof(i2sChunk), &bytesRead, 10 / portTICK_PERIOD_MS);
                }
                size_t sampleCount = bytesRead / sizeof(int32_t);
                size_t bytesAvail = RECORD_SIZE - audioDataSize;
                size_t maxSamplesToCopy = bytesAvail / sizeof(int16_t);
                if (sampleCount > maxSamplesToCopy) sampleCount = maxSamplesToCopy;

                int16_t* out = (int16_t*)(audioBuffer + audioDataSize);
                long chunkSum = 0;
                for (size_t i = 0; i < sampleCount; i++) {
                    int16_t pcm = convertI2SSampleToPCM16(i2sChunk[i]);
                    int level = abs(pcm);
                    if (level > recordPeakLevel) recordPeakLevel = level;
                    chunkSum += level;
                    out[i] = pcm;  // raw capture; server handles speech filtering
                }
                audioDataSize += sampleCount * sizeof(int16_t);
                int chunkAvgEnergy = (sampleCount > 0) ? (int)(chunkSum / (long)sampleCount) : 0;

                unsigned long nowMs = millis();
                if (chunkAvgEnergy < VAD_STOP_THRESHOLD) {
                    if (silenceStartMs == 0) silenceStartMs = nowMs;
                } else {
                    silenceStartMs = 0;
                }

                unsigned long minRecordBytes = (unsigned long)((I2S_SAMPLE_RATE * 2UL * MIN_RECORD_MS) / 1000UL);
                if (audioDataSize >= minRecordBytes && silenceStartMs > 0 && (nowMs - silenceStartMs) >= VAD_SILENCE_MS) {
                    stopBySilence = true;
                }

                if (DEBUG_SERIAL) {
                    if (nowMs - lastRecordProgressLogMs >= 500) {
                        float recSec = (float)(nowMs - recordStartMs) / 1000.0f;
                        float bufferedSec = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
                        float maxSec = (float)RECORD_SIZE / (float)(I2S_SAMPLE_RATE * 2);
                        Serial.printf("[REC] t=%.2fs | buffered=%.2fs/%.2fs | bytes=%u | peak=%d | avg=%d\n",
                                      recSec, bufferedSec, maxSec, (unsigned int)audioDataSize, recordPeakLevel, chunkAvgEnergy);
                        lastRecordProgressLogMs = nowMs;
                    }
                }
            } else {
                stopByBufferFull = true;
            }

            if (stopBySilence || stopByBufferFull) {
                currentState = STATE_PROCESSING;
                if (DEBUG_SERIAL) {
                    float finalSec = (float)audioDataSize / (float)(I2S_SAMPLE_RATE * 2);
                    Serial.printf("[REC] Done | duration=%.2fs | bytes=%u | peak=%d | reason=%s\n",
                                  finalSec, (unsigned int)audioDataSize, recordPeakLevel,
                                  stopBySilence ? "silence" : "buffer_full");
                }
                
                updateFaceAnimation(FACE_PROCESSING, true);
            }
            updateFaceAnimation(FACE_LISTENING);
            break;
        }
        
        case STATE_PROCESSING: {
            updateFaceAnimation(FACE_PROCESSING, true);
            if (DEBUG_SERIAL) Serial.printf("[AUDIO] Finished. Size: %d bytes\n", audioDataSize);

            if (postAudio()) {
                fetchCount++;
                tickerText = String(currentMessage);
                tickerOffset = 0;
                if (pendingAudioUrl.length() > 0) {
                    speakingFrame = 0;
                    playAudioFromUrl(pendingAudioUrl);
                    pendingAudioUrl = "";
                }
                // After speaking: idle face with ticker still scrolling
                // Cooldown: flush mic buffer to discard speaker echo/vibration
                flushI2SInput();
                delay(500);  // let speaker resonance die down
                flushI2SInput();
                vadHitCount = 0;  // reset VAD state after cooldown
                
                drawFaceAndTicker(FACE_IDLE, 0);
                currentState = STATE_DISPLAY;
                stateStartTime = millis();
            } else {
                showError("Voice Failed!", currentMessage);
                currentState = STATE_DISPLAY;
                stateStartTime = millis();
            }
            break;
        }

        
        case STATE_DISPLAY: {
            // Keep ticker scrolling while displaying answer
            updateFaceAnimation(FACE_IDLE);

            // Short timeout (2s) then back to standby — mic ready again
            bool displayTimeout = (millis() - stateStartTime > 2000);

            // VAD interrupt: user speaks before timeout
            bool vadInterrupt = false;
            if (!displayTimeout && rxChan != nullptr) {
                int32_t vadBuf[64];
                size_t vadBytes = 0;
                if (i2s_channel_read(rxChan, vadBuf, sizeof(vadBuf), &vadBytes, 0) == ESP_OK && vadBytes > 0) {
                    size_t vadSamples = vadBytes / sizeof(int32_t);
                    long vadSum = 0;
                    for (size_t i = 0; i < vadSamples; i++) vadSum += abs(convertI2SSampleToPCM16(vadBuf[i]));
                    int vadEnergy = (int)(vadSum / (long)vadSamples);
                    if (vadEnergy > VAD_THRESHOLD) {
                        vadHitCount++;
                        if (vadHitCount >= VAD_START_HITS) {
                            vadInterrupt = true;
                            if (DEBUG_SERIAL) Serial.printf("[VAD] Interrupt during DISPLAY (energy=%d)\n", vadEnergy);
                        }
                    } else if (vadHitCount > 0) {
                        vadHitCount--;
                    }
                }
            }

            if (displayTimeout || vadInterrupt) {
                currentState = vadInterrupt ? STATE_RECORDING : STATE_STANDBY;
                vadHitCount = 0;

                if (vadInterrupt) {
                    audioDataSize = 0;
                    recordStartMs = millis();
                    lastRecordProgressLogMs = recordStartMs;
                    recordPeakLevel = 0;
                    silenceStartMs = 0;
                    flushI2SInput();
                    updateFaceAnimation(FACE_LISTENING, true);
                } else {
                    tickerText = "";   // clear answer text when idle
                    tickerOffset = 0;
                    updateFaceAnimation(FACE_IDLE, true);
                    flushI2SInput();
                }
            }

            break;
        }
    }
    
    // ── WiFi Watchdog ───────────────────────────────────────
    static unsigned long lastWiFiCheck = 0;
    if (now - lastWiFiCheck >= 30000) {
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            if (DEBUG_SERIAL) Serial.println("[WiFi] Connection lost, will retry on next operation");
            ensureWiFi();
        }
        lastWiFiCheck = now;
    }

    if (USE_SOCKET_IO) {
        wsClient.loop();  // keep WebSocket alive, handle ping/pong
    }
    
    delay(10); // Small delay to prevent watchdog reset
}