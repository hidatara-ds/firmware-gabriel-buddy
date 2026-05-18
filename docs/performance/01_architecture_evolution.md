# Evolusi Arsitektur & Optimasi Performa Gabriel AI

Dokumen ini mencatat perjalanan evolusi *codebase* Gabriel AI, berfokus pada bagaimana kita mengatasi masalah latensi tinggi (delay) dan mencapai performa respons yang cepat dan stabil.

## Fase 1: Monolithic Pipeline (Versi Awal)
Pada versi awal, sistem bekerja secara sekuensial (tunggu-tungguan):
1. ESP32 merekam suara dan mengirim via WebSocket.
2. Server melakukan STT (Speech-to-Text).
3. Server memanggil Gemini LLM dan **menunggu seluruh jawaban (paragraf penuh) selesai**.
4. Server memanggil Google TTS, men-generate file audio MP3 untuk seluruh paragraf, lalu mengonversinya (via `pydub`) menjadi WAV 16kHz.
5. Server mengirim URL audio ke ESP32.

**❌ Masalah:** 
Delay sangat parah. Karena ESP32 harus menunggu *seluruh* proses di atas selesai, waktu jeda (time-to-first-audio) mencapai **10 hingga 13 detik**. User merasa bot lambat atau *freeze*.

---

## Fase 2: Sentence-Level Streaming & Direct PCM
Tujuan utama fase ini adalah memotong waktu tunggu secara drastis. Perubahan yang dilakukan:

1. **Pemecahan Kalimat (Sentence-Level Streaming):**
   Alih-alih menunggu seluruh teks selesai, server memecah *stream* jawaban Gemini berdasarkan tanda baca akhir kalimat (`.`, `!`, `?`). 
   Begitu satu kalimat terbentuk, kalimat itu langsung dikirim ke Google TTS dan URL-nya (event `audio_ready`) dikirim ke ESP32.
   *Hasil: ESP32 sudah mulai berbicara kalimat pertama, sementara server di belakang layar masih memproses kalimat kedua dan ketiga secara simultan.*

2. **Direct LINEAR16 (Bypass MP3 Conversion):**
   Konversi MP3 ke WAV memakan waktu ~0.5 - 1 detik. Kita ubah *request* Google TTS langsung meminta format `LINEAR16` (Raw PCM 16-bit 16kHz). Raw PCM ini kemudian kita "bungkus" (*wrap*) dengan *WAV Header* secara manual di Python (`struct.pack`) agar bisa langsung diputar oleh ESP32.

3. **Singleton API Clients:**
   Inisialisasi *client* Google Cloud Text-to-Speech dipindah menjadi *Singleton* (`_TTS_CLIENT`), menghemat ~200-400ms setiap pemanggilan karena tidak perlu re-autentikasi terus-menerus.

**✅ Hasil:** 
Time-to-First-Audio turun drastis 40% menjadi rata-rata **~4.8 detik**.

---

## Fase 3: Stabilisasi dan *Bug Fixes*
Dengan kecepatan yang sudah dicapai, muncul beberapa isu stabilitas baru:

### Isu 1: "Finish Reason: 2" (Crash pada Server)
*   **Gejala:** Koneksi WebSocket terputus, muncul error model "response did not complete successfully".
*   **Penyebab:** Hard-limit `max_output_tokens=100` membuat Vertex AI memotong jawaban yang kepanjangan, sehingga di-reject oleh sistem *safety/validation*.
*   **Solusi:** `max_output_tokens` dihapus (mengandalkan *System Prompt* untuk membatasi panjang jawaban) dan menambah `response_validation=False` pada `model.start_chat()`.

### Isu 2: Kata Indonesia Dieja (bukan Diucap)
*   **Gejala:** Kata seperti "deh", "nggak", "dong" dieja satu-persatu oleh AI (D-E-H).
*   **Penyebab:** Fungsi `detect_language` di-*hardcode* ke `"en"`, sehingga TTS menggunakan *voice* Inggris (`en-US-Journey-F`) untuk membaca teks Bahasa Indonesia.
*   **Solusi:** Mengaktifkan kembali `detect_language` menggunakan *Regex markers* kata-kata Indonesia, dan deteksi bahasa dilakukan pada **Answer** (bukan Question). Jika terdeteksi Indonesia, TTS menggunakan `id-ID-Wavenet-A`.

### Isu 3: Emoji di-TTS-kan Sendiri
*   **Gejala:** Muncul delay/audio pendek kosong karena bot men-generate emoji ("😉").
*   **Solusi:** Menambah instruksi di System Prompt: `NEVER use emoji in your responses. Your output is used for text-to-speech.` dan menambah filter regex di `websocket_handlers.py` untuk mengabaikan atau menggabungkan *fragment* yang terlalu pendek/hanya emoji.

### Isu 4: "Suara Tidak Terdeteksi" (False VAD Trigger)
*   **Gejala:** Setiap kali Gabriel selesai bicara, VAD langsung *trigger* merekam, lalu error karena tidak ada suara (hanya *noise*).
*   **Penyebab:** Mic INMP441 menangkap suara getaran/gema (echo) dari speaker MAX98357A miliknya sendiri sesaat setelah pemutaran audio selesai.
*   **Solusi (Firmware):** 
    1. Menaikkan `VAD_THRESHOLD` dari 250 ke 500.
    2. Menambahkan `delay(500)` dan `flushI2SInput()` tepat setelah fungsi `playAudioFromUrl` atau WebSocket Streaming selesai di ESP32, memberi waktu agar resonansi speaker hilang sebelum mic kembali aktif.

## Kesimpulan
Melalui 3 iterasi ini, *codebase* bergeser dari arsitektur *synchronous / blocking* menjadi arsitektur yang sangat reaktif dan *asynchronous*. Bot tidak hanya merespons lebih cepat, tetapi juga lebih kebal terhadap masalah *self-triggering* dan penanganan bahasa campuran.
