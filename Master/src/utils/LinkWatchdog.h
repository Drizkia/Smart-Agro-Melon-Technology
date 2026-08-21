#ifndef LINK_WATCHDOG_H
#define LINK_WATCHDOG_H

#include <stdint.h>

// Keputusan pengawas koneksi: apa yang harus dilakukan firmware ketika jalur
// WiFi/MQTT ke broker sedang putus.
//
// Latar: sebelum ini update() hanya `return` saat WiFi mati, dan connectWiFi()
// cuma dipanggil sekali dari begin(). Akibatnya satu kali putus WiFi = offline
// selamanya sampai dicabut-colok, sementara relay tetap terkunci menyala di
// lapangan dan perintah reset dari web tidak pernah sampai.
//
// Semua fungsi di sini murni dan constexpr — tanpa Arduino, tanpa I/O — supaya
// bisa diuji lewat static_assert di Master/test/LinkWatchdogTests.h.
enum LinkAction {
    LINK_OK,        // link hidup, tidak ada yang perlu dilakukan
    LINK_DEGRADED,  // putus, tapi masih dalam batas toleransi — cukup retry
    LINK_FAILSAFE,  // putus terlalu lama — matikan semua relay
    LINK_RESTART    // putus jauh lebih lama — restart board
};

// Sentinel untuk "belum pernah mencoba sejak link terakhir putus", sehingga
// percobaan pertama tidak perlu menunggu satu interval penuh.
constexpr uint32_t RETRY_NEVER_ATTEMPTED = 0UL;

// Selisih waktu millis() yang benar saat counter rollover (~49 hari).
// Aritmetika unsigned membuat pengurangan otomatis wrap dengan benar.
constexpr uint32_t elapsedSince(uint32_t thenMs, uint32_t nowMs) {
    return nowMs - thenMs;
}

// Apakah sudah waktunya mencoba menyambung ulang?
constexpr bool isRetryDue(uint32_t lastAttemptMs,
                          uint32_t nowMs,
                          uint32_t intervalMs) {
    return (lastAttemptMs == RETRY_NEVER_ATTEMPTED)
        ? true
        : (elapsedSince(lastAttemptMs, nowMs) >= intervalMs);
}

// Aksi yang harus diambil untuk keadaan link saat ini.
//
// failsafeMs / restartMs bernilai 0 berarti fitur tersebut dimatikan.
// LINK_RESTART menang atas LINK_FAILSAFE: kalau sudah selama itu putus, relay
// tetap ikut mati karena boot ulang memanggil RelayManager::allOff().
//
// Ditulis sebagai satu ekspresi return agar tetap valid constexpr di C++11,
// sama seperti isMinuteInsideWindow() di TimeWindow.h.
constexpr LinkAction decideLinkAction(bool linkUp,
                                      uint32_t msSinceLinkOk,
                                      uint32_t failsafeMs,
                                      uint32_t restartMs) {
    return linkUp
        ? LINK_OK
        : (restartMs != 0UL && msSinceLinkOk >= restartMs)
            ? LINK_RESTART
            : (failsafeMs != 0UL && msSinceLinkOk >= failsafeMs)
                ? LINK_FAILSAFE
                : LINK_DEGRADED;
}

#endif
