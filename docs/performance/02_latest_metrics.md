# Gabriel AI - Ringkasan Performa & Latensi Terbaru

Berdasarkan hasil pengujian di Serial Monitor dengan arsitektur **Sentence-Level Streaming**, berikut adalah ringkasan performa *pipeline* Gabriel AI dari saat user selesai bicara hingga audio pertama kali bersuara.

## 📊 Tabel Pengujian

| Pertanyaan / Perintah | Upload Audio | Waktu STT (Transkripsi) | **Waktu Tunggu (Audio Pertama Bunyi)** | Selesai Total (Semua Kalimat) |
| :--- | :--- | :--- | :--- | :--- |
| "Cuaca di Salatiga saat ini..." | 989 ms | +3.33 detik | **+5.14 detik** | +6.88 detik |
| "Nggak usah pakai dicek..." | 630 ms | +1.31 detik | **+7.46 detik** | +11.40 detik |
| "Pisah-pisah gitu enggak kejawab..."| 829 ms | +1.61 detik | **+4.88 detik** | +7.10 detik |
| "Hari ini hari apa?" | 743 ms | +1.43 detik | **+3.49 detik** | +4.90 detik |
| "Who is the president of..." | 958 ms | +1.77 detik | **+4.92 detik** | +8.89 detik |
| "Browser gambarnya Singa..." | 946 ms | +1.92 detik | **+4.80 detik** | +8.19 detik |
| "Browser yang namanya Brief..." | 534 ms | +1.35 detik | **+4.23 detik** | +6.64 detik |
| "50 + 50 + 50 dibagi 3..." | 1085 ms | +1.80 detik | **+4.82 detik** | +7.27 detik |
| "100 dikali 2..." | 1059 ms | +1.78 detik | **+3.94 detik** | +8.83 detik |

---

## 📈 Rata-Rata Performa (Average)

Dari data di atas, kita bisa menarik rata-rata kecepatan sistem:

1. **Upload Audio (ESP32 ke Server): `~860 ms`**
   *(Waktu yang dibutuhkan ESP32 untuk mengirim rekaman audio 2-5 detik via WebSocket).*
2. **Proses STT (Speech-to-Text): `~1.81 detik`**
   *(Waktu Server mengenali suara menjadi teks setelah audio diterima).*
3. **🔥 Waktu Tunggu Respons (Time-to-First-Audio): `~4.85 detik` 🔥**
   *(Ini adalah **jeda waktu (delay)** sejak user selesai bicara sampai Gabriel mulai ngomong kalimat pertamanya).*
4. **Selesai Total (Stream Done): `~7.78 detik`**
   *(Waktu total sampai seluruh kalimat jawaban selesai di-generate dan dikirim oleh server).*

> **Note:** Waktu tunggu (delay) turun drastis ~40% dibandingkan arsitektur Monolithic sebelumnya yang membutuhkan 10-13 detik. Karena server melakukan stream per kalimat, ESP32 bisa memutar audio pertama kali dalam < 5 detik, sementara server melanjutkan proses TTS kalimat berikutnya di latar belakang.
