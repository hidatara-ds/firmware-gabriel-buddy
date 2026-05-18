#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// WiFi
#define WIFI_SSID "Joglo Indoor"
#define WIFI_PASSWORD "rumahjoglo"
#define WIFI_TIMEOUT_MS 15000

// API
#define API_BASE_URL "https://gabriel-api-189789290221.us-central1.run.app"
#define API_ENDPOINT "/api/process-audio"
#define HTTP_TIMEOUT_MS 30000
#define USE_SOCKET_IO true // Use WebSocket for streaming pipeline
#define SOCKET_IO_RESPONSE_TIMEOUT_MS 90000

// I2S mic (INMP441) — original working values
#define I2S_SCK GPIO_NUM_5
#define I2S_WS GPIO_NUM_4
#define I2S_SD GPIO_NUM_6
#define I2S_SAMPLE_RATE 16000
#define I2S_SAMPLE_SHIFT 14
#define INMP441_USE_LEFT true
#define VAD_THRESHOLD 500
#define VAD_START_HITS 4
#define VAD_STOP_THRESHOLD 120
#define VAD_SILENCE_MS 700
#define MIN_UPLOAD_PEAK 500
#define MIC_DIAG_ON_BOOT false
#define MIC_DIAG_DURATION_MS 2500

// OLED (SSD1306 I2C)
#define OLED_SDA GPIO_NUM_8
#define OLED_SCL GPIO_NUM_9

// I2S Speaker (MAX98357A) — SD pin hard-wired to 3.3V
#define I2S_SPEAKER_BCLK GPIO_NUM_40
#define I2S_SPEAKER_LRC GPIO_NUM_41
#define I2S_SPEAKER_DIN GPIO_NUM_42
#define SPEAKER_VOLUME_PERCENT 75
#define SPEAKER_GAIN_PERCENT 100
#define AUDIO_STREAM_IDLE_TIMEOUT_MS 1200

// Runtime
#define RECORD_TIME 5
#define SCROLL_SPEED_MS 80
#define MESSAGE_DISPLAY_MS 8000
#define MAX_MSG_LENGTH 128
#define DEBUG_SERIAL true

#endif