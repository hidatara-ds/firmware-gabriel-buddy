// ============================================================
// display_face.h — OLED display, face animation, boot screen
// ============================================================
#ifndef DISPLAY_FACE_H
#define DISPLAY_FACE_H

// ── OLED Display Instance (SSD1306 128x64, I2C) ────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ── Display Layout Constants ────────────────────────────────
#define HEADER_HEIGHT   14
#define FOOTER_HEIGHT   10
#define BODY_Y_START    (HEADER_HEIGHT + 2)
#define BODY_HEIGHT     (64 - HEADER_HEIGHT - FOOTER_HEIGHT - 2)
#define CHAR_WIDTH      6
#define LINE_HEIGHT     10
#define MAX_CHARS_LINE  21  // 128 / 6 = 21 chars per line

// ── Face Mood ───────────────────────────────────────────────
enum FaceMood {
    FACE_IDLE,
    FACE_LISTENING,
    FACE_PROCESSING,
    FACE_SPEAKING,
};

// ── Blink / Animation State ────────────────────────────────
unsigned long nextBlinkMs = 0;
unsigned long blinkEndMs = 0;
unsigned long lastFaceDrawMs = 0;
bool eyesClosed = false;

// Forward declarations
void drawFaceAndTicker(FaceMood mood, int speakFrame);
void updateFaceAnimation(FaceMood mood, bool forceDraw = false);

// ═══════════════════════════════════════════════════════════
// BOOT ANIMATION
// ═══════════════════════════════════════════════════════════

void showBootAnimation() {
    // Frame 1: Logo fade-in
    for (int brightness = 0; brightness <= 255; brightness += 15) {
        display.setContrast(brightness);
        display.clearBuffer();
        display.setFont(u8g2_font_helvB12_tr);
        
        int textWidth = display.getStrWidth("GABRIEL");
        int x = (128 - textWidth) / 2;
        display.drawStr(x, 28, "GABRIEL");
        
        display.setFont(u8g2_font_5x7_tr);
        const char* subtitle = "Cloud Messenger";
        int subWidth = display.getStrWidth(subtitle);
        display.drawStr((128 - subWidth) / 2, 44, subtitle);
        
        display.sendBuffer();
        delay(20);
    }
    delay(800);

    // Frame 2: Cloud icon animation
    display.clearBuffer();
    display.setFont(u8g2_font_helvB12_tr);
    int tw = display.getStrWidth("GABRIEL");
    display.drawStr((128 - tw) / 2, 28, "GABRIEL");
    
    display.setFont(u8g2_font_5x7_tr);
    
    // Animated dots
    for (int i = 0; i < 3; i++) {
        display.clearBuffer();
        display.setFont(u8g2_font_helvB12_tr);
        display.drawStr((128 - tw) / 2, 28, "GABRIEL");
        display.setFont(u8g2_font_5x7_tr);
        
        char bootText[16];
        snprintf(bootText, sizeof(bootText), "Booting%.*s", i + 1, "...");
        int bw = display.getStrWidth(bootText);
        display.drawStr((128 - bw) / 2, 50, bootText);
        
        display.sendBuffer();
        delay(400);
    }
}

// ═══════════════════════════════════════════════════════════
// WiFi STATUS DISPLAY
// ═══════════════════════════════════════════════════════════

void showWiFiStatus(const char* status, int progress) {
    display.clearBuffer();
    
    // Header
    display.setFont(u8g2_font_5x7_tr);
    display.drawStr(4, 8, "WiFi Connecting...");
    
    // SSID
    display.setFont(u8g2_font_6x10_tr);
    char ssidLine[32];
    snprintf(ssidLine, sizeof(ssidLine), "SSID: %.16s", WIFI_SSID);
    display.drawStr(4, 28, ssidLine);
    
    // Status
    display.drawStr(4, 42, status);
    
    // Progress bar
    display.drawFrame(4, 50, 120, 8);
    if (progress > 0) {
        display.drawBox(4, 50, (120 * progress) / 100, 8);
    }
    
    display.sendBuffer();
}

// ═══════════════════════════════════════════════════════════
// FACE + TICKER RENDERING
// ═══════════════════════════════════════════════════════════

// Layout: face in rows 0-42, separator at 43, ticker in rows 44-63
void drawFaceAndTicker(FaceMood mood, int speakFrame) {
    display.clearBuffer();

    // ── Face frame (rows 1-41) ──────────────────────────────
    display.drawRFrame(4, 1, 120, 40, 5);

    // Eyes
    if (eyesClosed) {
        display.drawHLine(33, 18, 18);
        display.drawHLine(77, 18, 18);
    } else {
        int eyeH = (mood == FACE_LISTENING) ? 14 : 10;
        int eyeY = (mood == FACE_LISTENING) ?  9 : 12;
        display.drawRBox(33, eyeY, 18, eyeH, 3);
        display.drawRBox(77, eyeY, 18, eyeH, 3);
    }

    // Mouth
    switch (mood) {
        case FACE_LISTENING:
            display.drawDisc(64, 34, 3, U8G2_DRAW_ALL);
            break;
        case FACE_PROCESSING: {
            // Simple moving dot — left to right
            int px = 44 + (speakFrame % 5) * 10;
            display.drawDisc(px, 33, 3, U8G2_DRAW_ALL);
            // Static base line
            display.drawHLine(44, 33, 40);
            break;
        }
        case FACE_SPEAKING: {
            // Outline mouth (not filled) to avoid bleed artifacts
            switch (speakFrame % 4) {
                case 0: // closed — thin line
                    display.drawHLine(50, 35, 28);
                    break;
                case 1: // half open — small outline rect
                    display.drawRFrame(50, 31, 28, 6, 2);
                    break;
                case 2: // open — larger outline rect
                    display.drawRFrame(50, 28, 28, 10, 3);
                    break;
                case 3: // half open
                    display.drawRFrame(50, 31, 28, 6, 2);
                    break;
            }
            break;
        }
        case FACE_IDLE:
        default:
            display.drawLine(50, 34, 64, 38);
            display.drawLine(64, 38, 78, 34);
            break;
    }


    // ── Separator line ────────────────────────────────────
    display.drawHLine(0, 43, 128);

    // ── Ticker strip (rows 44-63) ────────────────────────
    display.setFont(u8g2_font_6x10_tr);
    if (tickerText.length() > 0) {
        // Scrolling answer text
        int x = 128 - tickerOffset;
        display.drawStr(x, 60, tickerText.c_str());
        // Draw second copy for seamless loop
        int tw = display.getStrWidth(tickerText.c_str());
        display.drawStr(x + tw + 16, 60, tickerText.c_str());
    } else {
        // Status label when idle
        display.setFont(u8g2_font_5x7_tr);
        const char* lbl = "STANDBY";
        if      (mood == FACE_LISTENING)   lbl = "LISTENING...";
        else if (mood == FACE_PROCESSING)  lbl = "THINKING...";
        else if (mood == FACE_SPEAKING)    lbl = "SPEAKING";
        int lw = display.getStrWidth(lbl);
        display.drawStr((128 - lw) / 2, 58, lbl);
    }

    display.sendBuffer();
}

void updateFaceAnimation(FaceMood mood, bool forceDraw) {
    unsigned long now = millis();
    if (nextBlinkMs == 0) nextBlinkMs = now + 1600 + random(0, 1400);

    if (!eyesClosed && now >= nextBlinkMs) {
        eyesClosed = true;
        blinkEndMs = now + 120;
    } else if (eyesClosed && now >= blinkEndMs) {
        eyesClosed = false;
        nextBlinkMs = now + 1400 + random(0, 2200);
    }

    // Advance processing/speaking animation frame every 200ms
    static unsigned long lastFrameMs = 0;
    if (now - lastFrameMs >= 200) {
        speakingFrame = (speakingFrame + 1) % 20;  // large modulus avoids frequent reset
        lastFrameMs = now;
    }

    // Advance ticker scroll every 35ms
    bool tickerMoved = false;
    if (tickerText.length() > 0 && (now - lastTickerMs) >= 35) {
        tickerOffset += 2;
        int tw = display.getStrWidth(tickerText.c_str());
        if (tickerOffset >= tw + 20) tickerOffset = 0;
        lastTickerMs = now;
        tickerMoved  = true;
    }

    if (forceDraw || tickerMoved || (now - lastFaceDrawMs) >= 80) {
        drawFaceAndTicker(mood, speakingFrame);
        lastFaceDrawMs = now;
    }

}

// ── Error Display ───────────────────────────────────────────
void showError(const char* title, const char* detail) {
    // Show error using face+ticker layout
    tickerText = String(title) + " - " + String(detail);
    tickerOffset = 0;
    drawFaceAndTicker(FACE_IDLE, 0);
}

#endif
