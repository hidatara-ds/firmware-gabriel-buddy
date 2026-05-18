// ============================================================
// wifi_manager.h — WiFi connection management
// ============================================================
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

bool connectWiFi() {
    if (DEBUG_SERIAL) Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    unsigned long startTime = millis();
    int dots = 0;
    
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > WIFI_TIMEOUT_MS) {
            if (DEBUG_SERIAL) Serial.println("[WiFi] ✗ Timeout!");
            showWiFiStatus("TIMEOUT!", 0);
            delay(1000);
            return false;
        }
        
        dots = ((millis() - startTime) * 100) / WIFI_TIMEOUT_MS;
        char statusText[32];
        snprintf(statusText, sizeof(statusText), "Connecting%.*s", (int)((millis() / 500) % 4), "...");
        showWiFiStatus(statusText, dots);
        delay(200);
    }
    
    wifiConnected = true;
    
    if (DEBUG_SERIAL) {
        Serial.printf("[WiFi] ✓ Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    }
    
    // Show success
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tr);
    display.drawStr(4, 20, "WiFi Connected!");
    
    char ipLine[32];
    snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());
    display.drawStr(4, 36, ipLine);
    
    char rssiLine[24];
    snprintf(rssiLine, sizeof(rssiLine), "RSSI: %d dBm", WiFi.RSSI());
    display.drawStr(4, 50, rssiLine);
    
    display.sendBuffer();
    delay(1500);
    
    return true;
}

bool ensureWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    
    wifiConnected = false;
    if (DEBUG_SERIAL) Serial.println("[WiFi] Disconnected, reconnecting...");
    return connectWiFi();
}

#endif
