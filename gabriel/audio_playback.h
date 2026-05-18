// ============================================================
// audio_playback.h — Audio playback from URL (WAV download + I2S)
// ============================================================
#ifndef AUDIO_PLAYBACK_H
#define AUDIO_PLAYBACK_H

// Parse WAV header properly — find the "data" chunk offset.
// Returns the byte offset where PCM data starts, or -1 on failure.
// This handles WAV files with extra chunks (fact, LIST, etc.) before "data".
int parseWavHeaderFromStream(WiFiClient* stream) {
    // Read the fixed RIFF header (12 bytes): "RIFF" + size + "WAVE"
    uint8_t riff[12];
    int got = 0;
    unsigned long t0 = millis();
    while (got < 12) {
        if (millis() - t0 > 2000) return -1;
        int r = stream->read(riff + got, 12 - got);
        if (r > 0) got += r;
        else delay(1);
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        if (DEBUG_SERIAL) Serial.println("[AUDIO] Not a valid WAV file");
        return -1;
    }

    // Walk chunks until we find "data"
    int totalSkipped = 12;
    for (int attempt = 0; attempt < 16; attempt++) {
        // Read 8-byte chunk header: 4-byte ID + 4-byte size (little-endian)
        uint8_t chunkHdr[8];
        got = 0;
        t0 = millis();
        while (got < 8) {
            if (millis() - t0 > 2000) return -1;
            int r = stream->read(chunkHdr + got, 8 - got);
            if (r > 0) got += r;
            else delay(1);
        }
        totalSkipped += 8;

        uint32_t chunkSize = (uint32_t)chunkHdr[4]
                           | ((uint32_t)chunkHdr[5] << 8)
                           | ((uint32_t)chunkHdr[6] << 16)
                           | ((uint32_t)chunkHdr[7] << 24);

        if (memcmp(chunkHdr, "data", 4) == 0) {
            // Found PCM data chunk — stream is now positioned at first PCM byte
            if (DEBUG_SERIAL) Serial.printf("[AUDIO] WAV data chunk found at offset %d, size=%u\n",
                                            totalSkipped, (unsigned int)chunkSize);
            return totalSkipped;
        }

        // Skip this chunk's payload (e.g. "fmt ", "fact", "LIST", etc.)
        if (DEBUG_SERIAL) {
            char id[5] = {0};
            memcpy(id, chunkHdr, 4);
            Serial.printf("[AUDIO] Skipping WAV chunk '%s' size=%u\n", id, (unsigned int)chunkSize);
        }
        uint32_t remaining = chunkSize;
        uint8_t skipBuf[64];
        t0 = millis();
        while (remaining > 0) {
            if (millis() - t0 > 3000) return -1;
            size_t want = (remaining < sizeof(skipBuf)) ? remaining : sizeof(skipBuf);
            int r = stream->read(skipBuf, want);
            if (r > 0) {
                remaining -= r;
                totalSkipped += r;
            } else {
                delay(1);
            }
        }
        // WAV chunks are word-aligned (pad byte if odd size)
        if (chunkSize & 1) {
            uint8_t pad;
            stream->read(&pad, 1);
            totalSkipped++;
        }
    }

    if (DEBUG_SERIAL) Serial.println("[AUDIO] WAV data chunk not found");
    return -1;
}

// ── Play WAV audio from URL via I2S speaker ─────────────────
void playAudioFromUrl(String url) {
    if (txChan == nullptr) { Serial.println("[AUDIO] txChan is null — speaker not init"); return; }
    if (url.length() == 0) { Serial.println("[AUDIO] URL is empty"); return; }
    Serial.printf("[AUDIO] Playing: %s\n", url.c_str());

    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[AUDIO] HTTP GET failed: %d\n", httpCode);
        http.end();
        return;
    }

    int contentLength = http.getSize();
    Serial.printf("[AUDIO] Content-Length: %d\n", contentLength);

    WiFiClient* stream = http.getStreamPtr();

    const size_t MAX_PSRAM_BYTES = 600 * 1024;
    const size_t MAX_HEAP_BYTES  = 180 * 1024;

    size_t allocSize = (contentLength > 0) ? (size_t)contentLength
                     : (psramFound() ? MAX_PSRAM_BYTES : MAX_HEAP_BYTES);

    uint8_t* rawBuf = nullptr;
    if (psramFound()) rawBuf = (uint8_t*)ps_malloc(allocSize);
    if (!rawBuf) {
        if (allocSize > MAX_HEAP_BYTES) allocSize = MAX_HEAP_BYTES;
        rawBuf = (uint8_t*)malloc(allocSize);
    }
    if (!rawBuf) {
        Serial.printf("[AUDIO] Buffer alloc FAILED: need %u bytes, free heap=%u\n",
                      (unsigned)allocSize, ESP.getFreeHeap());
        http.end(); return;
    }
    Serial.printf("[AUDIO] Buffer alloc OK: %u bytes\n", (unsigned)allocSize);

    // ── Download ──────────────────────────────────────────────
    size_t total = 0;
    uint8_t chunk[2048];
    const unsigned long DOWNLOAD_TIMEOUT_MS = 20000;
    unsigned long downloadStart = millis();

    while (total < allocSize) {
        if (millis() - downloadStart > DOWNLOAD_TIMEOUT_MS) {
            Serial.printf("[AUDIO] Download timeout after %u bytes\n", (unsigned)total);
            break;
        }
        int waited = 0;
        while (stream->available() == 0 && waited < 3000) {
            if (!http.connected()) goto download_done;
            delay(10); waited += 10;
        }
        if (stream->available() == 0) break;
        size_t want = stream->available();
        if (want > sizeof(chunk)) want = sizeof(chunk);
        if (want > allocSize - total) want = allocSize - total;
        size_t got = stream->readBytes(chunk, want);
        if (got > 0) { memcpy(rawBuf + total, chunk, got); total += got; }
        if (contentLength > 0 && total >= (size_t)contentLength) break;
    }
    download_done:
    http.end();
    Serial.printf("[AUDIO] Downloaded: %u bytes\n", (unsigned)total);

    if (contentLength > 0 && total < (size_t)contentLength) {
        Serial.printf("[AUDIO] Truncated! got=%u expected=%d — abort\n", (unsigned)total, contentLength);
        free(rawBuf); return;
    }
    if (total < 44) {
        Serial.printf("[AUDIO] Too small (%u bytes) — abort\n", (unsigned)total);
        free(rawBuf); return;
    }

    // ── Parse WAV header ──────────────────────────────────────
    if (memcmp(rawBuf, "RIFF", 4) != 0 || memcmp(rawBuf + 8, "WAVE", 4) != 0) {
        // Dump first 16 bytes for diagnosis
        Serial.printf("[AUDIO] Not a WAV! First bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
            rawBuf[0],rawBuf[1],rawBuf[2],rawBuf[3],rawBuf[4],rawBuf[5],rawBuf[6],rawBuf[7]);
        free(rawBuf); return;
    }

    // Read sample rate from fmt chunk (bytes 24-27 in standard WAV)
    uint32_t wavSampleRate = 16000;
    uint16_t wavChannels   = 1;
    uint16_t wavBitsPerSample = 16;
    int dataOffset = -1;
    int pos = 12;
    while (pos + 8 <= (int)total) {
        uint32_t sz = rawBuf[pos+4] | (rawBuf[pos+5]<<8) | (rawBuf[pos+6]<<16) | (rawBuf[pos+7]<<24);
        if (memcmp(rawBuf + pos, "fmt ", 4) == 0) {
            wavChannels    = rawBuf[pos+10] | (rawBuf[pos+11]<<8);
            wavSampleRate  = rawBuf[pos+12] | (rawBuf[pos+13]<<8)
                           | ((uint32_t)rawBuf[pos+14]<<16) | ((uint32_t)rawBuf[pos+15]<<24);
            wavBitsPerSample = rawBuf[pos+22] | (rawBuf[pos+23]<<8);
            Serial.printf("[AUDIO] WAV fmt: %uHz %uch %ubit\n",
                          (unsigned)wavSampleRate, (unsigned)wavChannels, (unsigned)wavBitsPerSample);
        }
        if (memcmp(rawBuf + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            break;
        }
        pos += 8 + (int)sz + (sz & 1 ? 1 : 0);
    }

    if (dataOffset < 0 || dataOffset >= (int)total) {
        Serial.println("[AUDIO] WAV data chunk not found — abort");
        free(rawBuf); return;
    }

    uint8_t* pcm    = rawBuf + dataOffset;
    size_t   pcmLen = total - dataOffset;
    if (pcmLen & 1) pcmLen--;
    Serial.printf("[AUDIO] PCM: offset=%d len=%u (%.2fs @ %uHz)\n",
        dataOffset, (unsigned)pcmLen,
        pcmLen / (float)(wavSampleRate * wavChannels * (wavBitsPerSample / 8)),
        (unsigned)wavSampleRate);

    // ── Reconfigure I2S clock if WAV sample rate differs ──────
    if (wavSampleRate != I2S_SAMPLE_RATE) {
        Serial.printf("[AUDIO] Reconfiguring I2S from %d to %u Hz\n", I2S_SAMPLE_RATE, (unsigned)wavSampleRate);
        i2s_channel_disable(txChan);
        i2s_std_clk_config_t newClk = I2S_STD_CLK_DEFAULT_CONFIG(wavSampleRate);
        i2s_channel_reconfig_std_clock(txChan, &newClk);
        i2s_channel_enable(txChan);
    }

    // ── Play PCM via I2S ──────────────────────────────────────
    const size_t SBUF = 4096;
    uint8_t* sbuf = (uint8_t*)malloc(SBUF);
    if (!sbuf) {
        Serial.printf("[AUDIO] sbuf malloc FAILED (free heap=%u) — abort\n", ESP.getFreeHeap());
        free(rawBuf); return;
    }

    // Silence warm-up
    memset(sbuf, 0, SBUF);
    size_t dummy = 0;
    i2s_channel_write(txChan, sbuf, SBUF, &dummy, pdMS_TO_TICKS(200));
    Serial.printf("[AUDIO] Starting playback: %u bytes PCM...\n", (unsigned)pcmLen);

    size_t offset2 = 0;
    unsigned long lastAnimMs = 0;
    while (offset2 < pcmLen) {
        size_t monoChunk = pcmLen - offset2;
        if (monoChunk > SBUF / 2) monoChunk = SBUF / 2;
        if (monoChunk & 1) monoChunk--;
        size_t stereoBytes = monoToStereoWithGain(pcm + offset2, monoChunk, sbuf, SBUF);
        if (stereoBytes > 0) {
            size_t written = 0;
            esp_err_t werr = i2s_channel_write(txChan, sbuf, stereoBytes, &written, pdMS_TO_TICKS(500));
            if (werr != ESP_OK) {
                Serial.printf("[AUDIO] i2s_channel_write err: %d (written=%u)\n", werr, (unsigned)written);
            }
        }
        // Animate speaking mouth + scroll ticker every 150ms
        // Also pump WS to receive next sentence chunks while playing current one
        unsigned long nowAnim = millis();
        if (nowAnim - lastAnimMs >= 150) {
            speakingFrame = (speakingFrame + 1) % 4;
            tickerOffset += 4;
            int tw2 = display.getStrWidth(tickerText.c_str());
            if (tickerOffset >= tw2 + 20) tickerOffset = 0;
            drawFaceAndTicker(FACE_SPEAKING, speakingFrame);
            lastAnimMs = nowAnim;
            // Receive queued WS messages while playing (next sentence arrives here)
            if (USE_SOCKET_IO && wsConnected) wsClient.loop();
        }
        offset2 += monoChunk;
    }


    // Silence tail flush
    const size_t SILENCE_FLUSH_BYTES = 32768;
    uint8_t* silenceBuf = (uint8_t*)calloc(SILENCE_FLUSH_BYTES, 1);
    if (silenceBuf) {
        size_t dummy2 = 0;
        i2s_channel_write(txChan, silenceBuf, SILENCE_FLUSH_BYTES, &dummy2, pdMS_TO_TICKS(500));
        free(silenceBuf);
    }
    delay(50);

    // Restore original I2S clock if we changed it
    if (wavSampleRate != I2S_SAMPLE_RATE) {
        i2s_channel_disable(txChan);
        i2s_std_clk_config_t origClk = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE);
        i2s_channel_reconfig_std_clock(txChan, &origClk);
        i2s_channel_enable(txChan);
    } else {
        i2s_channel_disable(txChan);
        delay(10);
        i2s_channel_enable(txChan);
    }

    Serial.printf("[AUDIO] Done | vol=%d%% gain=%d%%\n", SPEAKER_VOLUME_PERCENT, SPEAKER_GAIN_PERCENT);
    free(sbuf);
    free(rawBuf);
}

#endif
