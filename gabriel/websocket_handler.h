// ============================================================
// websocket_handler.h — Socket.IO/WebSocket protocol + streaming
// ============================================================
#ifndef WEBSOCKET_HANDLER_H
#define WEBSOCKET_HANDLER_H

// ── Audio Queue Helpers ─────────────────────────────────────
bool audioQueuePush(const String& url) {
    int next = (audioQueueTail + 1) % AUDIO_QUEUE_SIZE;
    if (next == audioQueueHead) return false;  // full
    audioUrlQueue[audioQueueTail] = url;
    audioQueueTail = next;
    return true;
}
bool audioQueuePop(String& out) {
    if (audioQueueHead == audioQueueTail) return false;  // empty
    out = audioUrlQueue[audioQueueHead];
    audioQueueHead = (audioQueueHead + 1) % AUDIO_QUEUE_SIZE;
    return true;
}
bool audioQueueIsEmpty() { return audioQueueHead == audioQueueTail; }
void audioQueueReset() { audioQueueHead = audioQueueTail = 0; }

// ── WebSocket Event Handler ─────────────────────────────────
// Handles Socket.IO over raw WebSocket:
//   • Engine.IO handshake (packet '0' = OPEN, '40' = CONNECT OK)
//   • Socket.IO message events ('42["message",{...}]')
//   • Binary audio frames sent by sendBIN
void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            wsConnected  = false;
            wsHandshook  = false;
            wsSessionId  = "";
            if (DEBUG_SERIAL) Serial.println("[WS] Disconnected");
            break;

        case WStype_CONNECTED:
            wsConnected = true;
            if (DEBUG_SERIAL) Serial.printf("[WS] TCP connected to %s\n", (char*)payload);
            // Engine.IO will immediately send "0{...}" OPEN packet — we wait for it.
            break;

        case WStype_TEXT: {
            if (length == 0) break;
            char* text = (char*)payload;
            if (DEBUG_SERIAL) Serial.printf("[WS] TEXT: %.120s\n", text);

            // Engine.IO OPEN packet: starts with '0'
            if (text[0] == '0') {
                // Reply with Socket.IO connect: '40'
                wsClient.sendTXT("40");
                break;
            }

            // Socket.IO EVENT packet: starts with '42'
            if (length >= 2 && text[0] == '4' && text[1] == '2') {
                // text looks like: 42["message","{...}"]
                char* jsonPart = text + 2;  // skip '42'
                JsonDocument root;
                if (deserializeJson(root, jsonPart) != DeserializationError::Ok) break;
                if (!root.is<JsonArray>()) break;
                JsonArray arr = root.as<JsonArray>();
                if (arr.size() < 2) break;
                const char* eventName = arr[0] | "";
                if (strcmp(eventName, "message") != 0) break;

                // Inner payload can be a JSON string or object
                JsonDocument msgDoc;
                JsonVariant msgVariant = arr[1];
                if (msgVariant.is<const char*>()) {
                    if (deserializeJson(msgDoc, msgVariant.as<const char*>()) != DeserializationError::Ok) break;
                } else {
                    msgDoc.set(msgVariant);
                }

                const char* msgType = msgDoc["type"] | "";

                if (strcmp(msgType, "connected") == 0) {
                    wsSessionId = String(msgDoc["session_id"] | "");
                    wsHandshook = true;
                    if (DEBUG_SERIAL) Serial.printf("[WS] Session: %s\n", wsSessionId.c_str());
                }
                else if (strcmp(msgType, "processing") == 0) {
                    if (DEBUG_SERIAL) Serial.printf("[WS] Server processing audio... (+%lu ms)\n", millis() - lastRequestMs);
                }
                else if (strcmp(msgType, "stt_result") == 0) {
                    const char* txt = msgDoc["text"] | "";
                    if (DEBUG_SERIAL) Serial.printf("[WS] STT (+%lu ms): %s\n", millis() - lastRequestMs, txt);
                }
                // ── Streaming: server sends one audio_ready per sentence ──
                else if (strcmp(msgType, "audio_ready") == 0) {
                    const char* url = msgDoc["audio_url"] | "";
                    if (strlen(url) > 0) {
                        String fullUrl = String(url);
                        if (!fullUrl.startsWith("http")) fullUrl = String(API_BASE_URL) + fullUrl;
                        audioQueuePush(fullUrl);
                    }
                    // Accumulate text into ticker
                    const char* ct = msgDoc["text"] | "";
                    if (strlen(ct) > 0) {
                        if (tickerText.length() == 0) tickerText = String(ct);
                        else tickerText += String(" ") + String(ct);
                        tickerOffset = 0;
                    }
                    if (DEBUG_SERIAL) Serial.printf("[WS] audio_ready (+%lu ms): %s\n", millis() - lastRequestMs, url);
                }
                // ── Streaming: server done sending all sentences ──
                else if (strcmp(msgType, "done") == 0) {
                    const char* ans = msgDoc["full_answer"] | "";
                    if (strlen(ans) > 0) {
                        strncpy(currentMessage, ans, MAX_MSG_LENGTH - 1);
                        currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    }
                    strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                    wsStreamDone = true;
                    if (DEBUG_SERIAL) Serial.printf("[WS] Stream done (+%lu ms)\n", millis() - lastRequestMs);
                }
                // ── Legacy: single result packet (fallback) ──
                else if (strcmp(msgType, "result") == 0) {
                    strncpy(currentMessage, msgDoc["answer"] | "No message", MAX_MSG_LENGTH - 1);
                    currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    strncpy(currentCategory, "chat", sizeof(currentCategory) - 1);
                    const char* audioUrl = msgDoc["audio_url"] | "";
                    if (strlen(audioUrl) > 0) {
                        String fu = String(audioUrl);
                        if (!fu.startsWith("http")) fu = String(API_BASE_URL) + fu;
                        audioQueuePush(fu);
                    }
                    wsStreamDone = true;
                    if (DEBUG_SERIAL) Serial.printf("[WS] Answer(legacy): %s\n", currentMessage);
                }
                else if (strcmp(msgType, "error") == 0) {
                    strncpy(currentMessage, msgDoc["message"] | "WS error", MAX_MSG_LENGTH - 1);
                    currentMessage[MAX_MSG_LENGTH - 1] = '\0';
                    strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
                    if (DEBUG_SERIAL) Serial.printf("[WS] Error: %s\n", currentMessage);
                }
                break;
            }

            // Engine.IO PING? Reply PONG
            if (text[0] == '2') { wsClient.sendTXT("3"); break; }
            break;
        }

        case WStype_ERROR:
            if (DEBUG_SERIAL) Serial.printf("[WS] Error: %.*s\n", (int)length, (char*)payload);
            break;

        default: break;
    }
}

// ── Socket.IO message emit helper ───────────────────────────
// Wraps a JSON payload as Socket.IO event: 42["message", payload]
void wsSendMessage(const char* jsonPayload) {
    // Build: 42["message",<payload>]
    String frame = "42[\"message\",";
    frame += jsonPayload;
    frame += "]";
    wsClient.sendTXT(frame);
    if (DEBUG_SERIAL) Serial.printf("[WS] SEND: %.80s\n", frame.c_str());
}

bool ensureWsConnected() {
    if (!USE_SOCKET_IO) return false;
    if (!ensureWiFi()) return false;
    if (wsConnected && wsHandshook) return true;

    String host = getApiHost();
    if (host.length() == 0) return false;

    if (DEBUG_SERIAL) Serial.printf("[WS] Connecting to %s...\n", host.c_str());

    // Socket.IO endpoint — EIO=4 is Engine.IO v4 (Flask-SocketIO default)
    String path = "/socket.io/?EIO=4&transport=websocket";
    if (isApiTls()) {
        wsClient.beginSSL(host.c_str(), 443, path.c_str());
    } else {
        wsClient.begin(host.c_str(), 80, path.c_str());
    }
    wsClient.onEvent(wsEvent);
    wsClient.setReconnectInterval(4000);
    wsClient.enableHeartbeat(25000, 10000, 3);  // ping every 25s, pong timeout 10s

    unsigned long start = millis();
    while (millis() - start < 15000) {
        wsClient.loop();
        if (wsConnected && wsHandshook) {
            if (DEBUG_SERIAL) Serial.println("[WS] Handshake complete!");
            return true;
        }
        delay(10);
    }
    if (DEBUG_SERIAL) Serial.println("[WS] Connection timeout");
    return false;
}

bool postAudioViaWebSocket() {
    if (!ensureWsConnected()) return false;
    if (audioBuffer == nullptr || audioDataSize < 2) return false;

    // Reset streaming state
    audioQueueReset();
    wsStreamDone     = false;
    currentCategory[0] = '\0';
    tickerText       = "";
    tickerOffset     = 0;

    unsigned long uploadStartMs = millis();
    if (DEBUG_SERIAL) Serial.printf("[WS] Sending %u bytes audio...\n", (unsigned)audioDataSize);

    // Send PCM chunks as base64 JSON (larger chunks = fewer frames = faster upload)
    const size_t CHUNK_PCM = 16384;
    size_t offset = 0;
    while (offset < audioDataSize) {
        size_t n = audioDataSize - offset;
        if (n > CHUNK_PCM) n = CHUNK_PCM;
        String b64Chunk;
        if (!base64Encode(audioBuffer + offset, n, b64Chunk)) {
            if (DEBUG_SERIAL) Serial.println("[WS] base64 encode failed");
            return false;
        }
        String frame = "42[\"message\",{\"type\":\"audio_chunk\",\"data\":\"";
        frame += b64Chunk;
        frame += "\"}]";
        wsClient.sendTXT(frame);
        wsClient.loop();
        offset += n;
    }
    wsSendMessage("{\"type\":\"audio_end\"}");
    lastRequestMs = millis();
    if (DEBUG_SERIAL) Serial.printf("[WS] Audio sent in %lu ms. Waiting for response...\n", lastRequestMs - uploadStartMs);

    // ── Streaming play loop ───────────────────────────────────────────────
    // As audio_ready events arrive, play each sentence immediately.
    // wsClient.loop() inside playAudioFromUrl() receives next sentences while playing.
    unsigned long start = millis();
    while (millis() - start < (unsigned long)SOCKET_IO_RESPONSE_TIMEOUT_MS) {
        wsClient.loop();
        updateFaceAnimation(FACE_PROCESSING);

        if (strcmp(currentCategory, "error") == 0) return false;

        // Play any ready sentence
        String nextUrl;
        while (audioQueuePop(nextUrl)) {
            speakingFrame = 0;
            drawFaceAndTicker(FACE_SPEAKING, 0);
            playAudioFromUrl(nextUrl);  // pumps wsClient.loop() internally every 150ms
            // Brief pump after each sentence
            for (int i = 0; i < 8; i++) { wsClient.loop(); delay(8); }
        }

        // Exit when server done AND queue drained
        if (wsStreamDone && audioQueueIsEmpty()) {
            // Cooldown: flush mic buffer to discard speaker echo/vibration
            flushI2SInput();
            delay(500);
            flushI2SInput();
            drawFaceAndTicker(FACE_IDLE, 0);
            return true;
        }
        delay(5);
    }

    strncpy(currentMessage, "WebSocket timeout", MAX_MSG_LENGTH - 1);
    strncpy(currentCategory, "error", sizeof(currentCategory) - 1);
    return false;
}

#endif
