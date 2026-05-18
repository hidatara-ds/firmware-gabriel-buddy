// ============================================================
// i2s_driver.h — I2S hardware init and helpers
// ============================================================
#ifndef I2S_DRIVER_H
#define I2S_DRIVER_H

// ── Flush stale samples from RX FIFO/DMA ────────────────────
void flushI2SInput() {
    if (rxChan == nullptr) return;

    uint8_t flushBuf[512];
    size_t bytesRead = 0;
    while (i2s_channel_read(rxChan, flushBuf, sizeof(flushBuf), &bytesRead, 0) == ESP_OK && bytesRead > 0) {
        // Drain stale samples from RX FIFO/DMA.
    }
}

// ── Mic Diagnostic ──────────────────────────────────────────
void runMicDiagnostic(uint32_t durationMs) {
    if (rxChan == nullptr) {
        Serial.println("[MIC] Diagnostic skipped: rxChan null");
        return;
    }

    Serial.printf("[MIC] Diagnostic start: %u ms\n", (unsigned int)durationMs);
    uint32_t startMs = millis();
    uint64_t sumAbs = 0;
    uint32_t totalSamples = 0;
    int peak = 0;
    uint32_t nonZeroCount = 0;
    uint32_t nearClipCount = 0;

    while ((millis() - startMs) < durationMs) {
        int32_t chunk[256];
        size_t bytesRead = 0;
        i2s_channel_read(rxChan, chunk, sizeof(chunk), &bytesRead, 50 / portTICK_PERIOD_MS);
        size_t sampleCount = bytesRead / sizeof(int32_t);
        for (size_t i = 0; i < sampleCount; i++) {
            int level = abs(convertI2SSampleToPCM16(chunk[i]));
            sumAbs += level;
            totalSamples++;
            if (level > peak) peak = level;
            if (level > 8) nonZeroCount++;
            if (level > 30000) nearClipCount++;
        }
    }

    int avgAbs = (totalSamples > 0) ? (int)(sumAbs / totalSamples) : 0;
    float nonZeroPct = (totalSamples > 0) ? (100.0f * nonZeroCount / totalSamples) : 0.0f;
    float clipPct = (totalSamples > 0) ? (100.0f * nearClipCount / totalSamples) : 0.0f;

    Serial.printf("[MIC] Samples=%u avgAbs=%d peak=%d nonZero=%.1f%% nearClip=%.2f%%\n",
                  (unsigned int)totalSamples, avgAbs, peak, nonZeroPct, clipPct);

    if (totalSamples < 100) {
        Serial.println("[MIC] FAIL: almost no samples read. Check I2S pins/clock.");
    } else if (peak < 100 || avgAbs < 20) {
        Serial.println("[MIC] FAIL: signal too low/flat. Check mic wiring, VDD/GND, SD pin.");
    } else if (clipPct > 20.0f) {
        Serial.println("[MIC] WARN: signal clipping heavily. Lower gain / check scaling.");
    } else {
        Serial.println("[MIC] OK: microphone signal detected.");
    }
}

// ── Init I2S Microphone (RX channel on I2S_NUM_0) ──────────
void initI2SMic() {
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t i2sErr = i2s_new_channel(&chanCfg, NULL, &rxChan);
    if (i2sErr != ESP_OK) {
        Serial.printf("[ERROR] i2s_new_channel failed: %d\n", i2sErr);
        return;
    }

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    stdCfg.slot_cfg.slot_mask = INMP441_USE_LEFT ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;

    if (rxChan != nullptr) {
        i2sErr = i2s_channel_init_std_mode(rxChan, &stdCfg);
        if (i2sErr != ESP_OK) {
            Serial.printf("[ERROR] i2s_channel_init_std_mode failed: %d\n", i2sErr);
        }

        i2sErr = i2s_channel_enable(rxChan);
        if (i2sErr != ESP_OK) {
            Serial.printf("[ERROR] i2s_channel_enable failed: %d\n", i2sErr);
        }

        flushI2SInput();
    }
}

// ── Init I2S Speaker (TX channel on I2S_NUM_1) ──────────────
void initI2SSpeaker() {
    i2s_chan_config_t txChanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    txChanCfg.dma_desc_num  = 8;   // increased from default 6
    txChanCfg.dma_frame_num = 512;  // increased from default 240 for larger buffer

    esp_err_t i2sErr = i2s_new_channel(&txChanCfg, &txChan, NULL);
    if (i2sErr != ESP_OK) {
        Serial.printf("[SPEAKER] i2s_new_channel failed: %d\n", i2sErr);
        return;
    }

    i2s_std_config_t txStdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SPEAKER_BCLK,
            .ws = I2S_SPEAKER_LRC,
            .dout = I2S_SPEAKER_DIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    i2sErr = i2s_channel_init_std_mode(txChan, &txStdCfg);
    if (i2sErr == ESP_OK) {
        i2s_channel_enable(txChan);
        Serial.println("[SPEAKER] OK: I2S TX initialized");
    } else {
        Serial.printf("[SPEAKER] i2s_channel_init_std_mode failed: %d\n", i2sErr);
    }
}

#endif
